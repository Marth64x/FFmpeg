/*
 * RCWT (Raw Captions With Time) demuxer
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * RCWT (Raw Captions With Time) is a format native to ccextractor, a commonly
 * used open source tool for processing 608/708 Closed Captions (CC) sources.
 * It can be used to archive the original, raw CC bitstream and to produce
 * a source file for later CC processing or conversion. As a result,
 * it also allows for interopability with ccextractor for processing CC data
 * extracted via ffmpeg. The format is simple to parse and can be used
 * to retain all lines and variants of CC.
 *
 * This demuxer implements the specification as of March 2024, which has
 * been stable and unchanged since April 2014.
 *
 * A free specification of RCWT can be found here:
 * @url{https://github.com/CCExtractor/ccextractor/blob/master/docs/BINARY_FILE_FORMAT.TXT}
 */

#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "subtitles.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"

#define RCWT_CLUSTER_MAX_BLOCKS             65535
#define RCWT_BLOCK_SIZE                     3
#define RCWT_HEADER_SIZE                    11

typedef struct RCWTContext {
    FFDemuxSubtitlesQueue q;
} RCWTContext;

static int rcwt_read_header(AVFormatContext *avf)
{
    RCWTContext *rcwt = avf->priv_data;

    AVPacket      *sub = NULL;
    AVStream      *st;
    uint8_t       header[RCWT_HEADER_SIZE] = {0};
    int           nb_bytes = 0;

    int64_t       cluster_pts = AV_NOPTS_VALUE;
    int           cluster_nb_blocks = 0;
    int           cluster_size = 0;
    uint8_t       *cluster_buf;

    /* validate the header */
    nb_bytes = avio_read(avf->pb, header, RCWT_HEADER_SIZE);
    if (nb_bytes != RCWT_HEADER_SIZE || AV_RB16(header) != 0xCCCC || header[2] != 0xED) {
        av_log(avf, AV_LOG_ERROR, "Input is not an RCWT file\n");
        return AVERROR_INVALIDDATA;
    }

    if ((header[3] != 0xCC && header[3] != 0xFF) || header[4] != 0x00) {
        av_log(avf, AV_LOG_ERROR, "Input writing application is not supported, only "
                                  "0xCC00 (ccextractor) or 0xFF00 (FFmpeg) are compatible\n");
        return AVERROR_INVALIDDATA;
    }

    if (AV_RB16(header + 6) != 0x0001) {
        av_log(avf, AV_LOG_ERROR, "Input RCWT version is not compatible "
                                  "(only version 0.001 is known)\n");
        return AVERROR_INVALIDDATA;
    }

    if (header[3] == 0xFF && header[5] != 0x60) {
        av_log(avf, AV_LOG_ERROR, "Input was written by a different version of FFmpeg "
                                  "and unsupported, consider upgrading\n");
        return AVERROR_INVALIDDATA;
    }

    /* setup AVStream */
    st = avformat_new_stream(avf, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id   = AV_CODEC_ID_EIA_608;

    avpriv_set_pts_info(st, 64, 1, 1000);

    /* demux */
    while (!avio_feof(avf->pb)) {
        cluster_pts       = avio_rl64(avf->pb);
        cluster_nb_blocks = avio_rl16(avf->pb);
        if (cluster_nb_blocks == 0)
            continue;

        cluster_size      = cluster_nb_blocks * RCWT_BLOCK_SIZE;
        cluster_buf       = av_calloc(cluster_nb_blocks, RCWT_BLOCK_SIZE);
        if (!cluster_buf)
            return AVERROR(ENOMEM);

        nb_bytes          = avio_read(avf->pb, cluster_buf, cluster_size);
        if (nb_bytes != cluster_size) {
            av_freep(&cluster_buf);
            av_log(avf, AV_LOG_ERROR, "Input cluster has invalid size "
                                      "(expected=%d actual=%d pos=%ld)\n",
                                      cluster_size, nb_bytes, avio_tell(avf->pb));
            return AVERROR_INVALIDDATA;
        }

        sub = ff_subtitles_queue_insert(&rcwt->q, cluster_buf, cluster_size, 0);
        if (!sub) {
            av_freep(&cluster_buf);
            return AVERROR(ENOMEM);
        }

        sub->pos = avio_tell(avf->pb);
        sub->pts = cluster_pts;

        av_freep(&cluster_buf);
        cluster_buf = NULL;
    }

    ff_subtitles_queue_finalize(avf, &rcwt->q);

    return 0;
}

static int rcwt_probe(const AVProbeData *p)
{
    return p->buf_size > RCWT_HEADER_SIZE &&
           AV_RB16(p->buf) == 0xCCCC && AV_RB8(p->buf + 2) == 0xED ? 50 : 0;
}

const FFInputFormat ff_rcwt_demuxer = {
    .p.name         = "rcwt",
    .p.long_name    = NULL_IF_CONFIG_SMALL("RCWT (Raw Captions With Time)"),
    .p.extensions   = "bin",
    .p.flags        = AVFMT_TS_DISCONT,
    .priv_data_size = sizeof(RCWTContext),
    .flags_internal = FF_FMT_INIT_CLEANUP,
    .read_probe     = rcwt_probe,
    .read_header    = rcwt_read_header,
    .read_packet    = ff_subtitles_read_packet,
    .read_seek2     = ff_subtitles_read_seek,
    .read_close     = ff_subtitles_read_close
};
