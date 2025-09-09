/* SPDX-License-Identifier: Apache-2.0 */
/*
 * V4L2/FILE to MPP UDP encoder sample.
 *
 * This example captures NV12 frames either from a V4L2 camera or from a raw
 * NV12 file in "test" mode.  Frames are encoded using Rockchip MPP as either
 * H.264 or H.265 and streamed over UDP.  Runtime options expose bitrate, codec
 * type, GOP size, ROI region and intra refresh configuration.  Camera frames
 * are passed to MPP via DMABUF to minimize latency.  The application writes
 * detailed bitrate statistics for every encoded frame to a log file.
 *
 * The code is intentionally verbose and prints debug information for almost
 * every operation to aid experimentation and troubleshooting.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#define MODULE_TAG "mpi_v4l2_udp_test"

extern "C" {
#include "rk_mpi.h"
#include "rk_venc_cmd.h"
#include "mpp_buffer.h"
}

#define MAX_BUFFERS 4

#define DBG(fmt, ...)                                                         \
    fprintf(stderr, "[DBG] %s:%d: " fmt "\n", __FUNCTION__, __LINE__,       \
            ##__VA_ARGS__)

struct V4L2Buffer {
    int fd;       // exported DMA buffer fd
    void *start;  // mmap'd address for debug access
    size_t length;
};

struct AppCfg {
    std::string device;
    std::string ip;
    std::string input_file;    // when set, use file instead of camera
    std::string log_path;
    int port;
    RK_U32 width;
    RK_U32 height;
    RK_U32 fps;
    RK_U32 gop;
    RK_U32 bitrate;
    MppCodingType type;
    bool intra_refresh;
    bool roi_enabled;
    MppEncROICfg roi_cfg;
};

static void cfg_set_default(AppCfg &cfg) {
    cfg.device = "/dev/video0";
    cfg.ip = "127.0.0.1";
    cfg.port = 5000;
    cfg.width = 640;
    cfg.height = 480;
    cfg.fps = 30;
    cfg.gop = 60;
    cfg.bitrate = 1000000;
    cfg.type = MPP_VIDEO_CodingAVC;
    cfg.intra_refresh = false;
    cfg.roi_enabled = false;
    memset(&cfg.roi_cfg, 0, sizeof(cfg.roi_cfg));
    cfg.log_path = "bitrate_log.txt";
    cfg.input_file.clear();
}

static void print_usage(const char *prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  -d <device>       V4L2 device (default /dev/video0)\n"
              << "  -a <ip>           UDP destination IP (default 127.0.0.1)\n"
              << "  -p <port>         UDP destination port (default 5000)\n"
              << "  -w <width>        capture width\n"
              << "  -h <height>       capture height\n"
              << "  -f <fps>          frame rate\n"
              << "  -b <bitrate>      target bitrate (bps)\n"
              << "  -g <gop>          GOP length\n"
              << "  -t <h264|h265>    codec type\n"
              << "  -r                enable intra refresh\n"
              << "  -R x,y,w,h        ROI region\n"
              << "  -l <file>         bitrate log file\n"
              << "  -i <file>         read raw NV12 frames from file instead of camera\n";
}

static int cfg_parse(int argc, char **argv, AppCfg &cfg) {
    int c;
    while ((c = getopt(argc, argv, "d:a:p:w:h:f:b:g:t:rR:l:i:")) != -1) {
        switch (c) {
        case 'd':
            cfg.device = optarg;
            break;
        case 'a':
            cfg.ip = optarg;
            break;
        case 'p':
            cfg.port = atoi(optarg);
            break;
        case 'w':
            cfg.width = atoi(optarg);
            break;
        case 'h':
            cfg.height = atoi(optarg);
            break;
        case 'f':
            cfg.fps = atoi(optarg);
            break;
        case 'b':
            cfg.bitrate = atoi(optarg);
            break;
        case 'g':
            cfg.gop = atoi(optarg);
            break;
        case 't':
            if (!strcmp(optarg, "h265"))
                cfg.type = MPP_VIDEO_CodingHEVC;
            else
                cfg.type = MPP_VIDEO_CodingAVC;
            break;
        case 'r':
            cfg.intra_refresh = true;
            break;
        case 'R':
            if (sscanf(optarg, "%hu,%hu,%hu,%hu",
                       &cfg.roi_cfg.regions[0].x, &cfg.roi_cfg.regions[0].y,
                       &cfg.roi_cfg.regions[0].w, &cfg.roi_cfg.regions[0].h) ==
                4) {
                cfg.roi_cfg.number = 1;
                cfg.roi_enabled = true;
            }
            break;
        case 'l':
            cfg.log_path = optarg;
            break;
        case 'i':
            cfg.input_file = optarg;
            break;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static int v4l2_setup(const AppCfg &cfg, std::vector<V4L2Buffer> &bufs) {
    int fd = open(cfg.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open v4l2");
        return -1;
    }
    DBG("Opened V4L2 device %s", cfg.device.c_str());

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cfg.width;
    fmt.fmt.pix.height = cfg.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        close(fd);
        return -1;
    }
    DBG("Configured format %ux%u", cfg.width, cfg.height);

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = cfg.fps;
    ioctl(fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = MAX_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        close(fd);
        return -1;
    }

    bufs.resize(req.count);
    for (RK_U32 i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = req.type;
        buf.memory = req.memory;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            close(fd);
            return -1;
        }
        bufs[i].length = buf.length;
        bufs[i].start =
            mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 buf.m.offset);
        if (bufs[i].start == MAP_FAILED) {
            perror("mmap");
            close(fd);
            return -1;
        }

        struct v4l2_exportbuffer exp;
        memset(&exp, 0, sizeof(exp));
        exp.type = req.type;
        exp.index = i;
        exp.flags = O_CLOEXEC;
        if (ioctl(fd, VIDIOC_EXPBUF, &exp) < 0) {
            perror("VIDIOC_EXPBUF");
            close(fd);
            return -1;
        }
        bufs[i].fd = exp.fd;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            close(fd);
            return -1;
        }
        DBG("Queued buffer %u fd %d", i, bufs[i].fd);
    }

    enum v4l2_buf_type type = (enum v4l2_buf_type)req.type;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
        perror("VIDIOC_STREAMON");
    DBG("Streaming on");
    return fd;
}

static MPP_RET mpp_setup(const AppCfg &cfg, MppCtx &ctx, MppApi *&mpi,
                         MppBuffer &frm_buf, size_t frame_size,
                         bool need_frame_buf) {
    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret) return ret;
    DBG("MPP context created");

    ret = mpp_init(ctx, MPP_CTX_ENC, cfg.type);
    if (ret) return ret;
    DBG("MPP encoder init done");

    if (need_frame_buf) {
        ret = mpp_buffer_get(NULL, &frm_buf, frame_size);
        if (ret) return ret;
        DBG("Allocated frame buffer size %zu", frame_size);
    }

    MppEncCfg enc_cfg = NULL;
    ret = mpp_enc_cfg_init(&enc_cfg);
    if (ret) return ret;

    mpp_enc_cfg_set_s32(enc_cfg, "prep:width", cfg.width);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:height", cfg.height);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:hor_stride", cfg.width);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:ver_stride", cfg.height);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:format", MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_num", cfg.fps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_den", 1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_num", cfg.fps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_den", 1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:gop", cfg.gop);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bitrate", cfg.bitrate);

    if (cfg.intra_refresh)
        mpp_enc_cfg_set_s32(enc_cfg, "intra:refresh", 1);

    ret = mpi->control(ctx, MPP_ENC_SET_CFG, enc_cfg);
    mpp_enc_cfg_deinit(enc_cfg);
    if (ret) {
        fprintf(stderr, "set cfg failed\n");
        return ret;
    }

    if (cfg.roi_enabled)
        mpi->control(ctx, MPP_ENC_SET_ROI_CFG,
                     const_cast<MppEncROICfg *>(&cfg.roi_cfg));

    DBG("MPP encoder configured");
    return MPP_OK;
}

int main(int argc, char **argv) {
    AppCfg cfg;
    cfg_set_default(cfg);
    if (cfg_parse(argc, argv, cfg))
        return -1;

    size_t frame_size = cfg.width * cfg.height * 3 / 2;

    FILE *input_fp = NULL;
    std::vector<V4L2Buffer> v4l2_bufs;
    int v4l2_fd = -1;
    if (!cfg.input_file.empty()) {
        input_fp = fopen(cfg.input_file.c_str(), "rb");
        if (!input_fp) {
            perror("open input file");
            return -1;
        }
        DBG("Using input file %s", cfg.input_file.c_str());
    } else {
        v4l2_fd = v4l2_setup(cfg, v4l2_bufs);
        if (v4l2_fd < 0)
            return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    addr.sin_addr.s_addr = inet_addr(cfg.ip.c_str());

    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    MppBuffer frm_buf = NULL;
    bool use_file = !cfg.input_file.empty();
    if (mpp_setup(cfg, ctx, mpi, frm_buf, frame_size, use_file)) {
        fprintf(stderr, "mpp setup failed\n");
        return -1;
    }

    FILE *log = fopen(cfg.log_path.c_str(), "w");
    struct timeval start;
    gettimeofday(&start, NULL);
    RK_U64 total_bits = 0;
    RK_S32 frame_id = 0;

    while (1) {
        MppBuffer use_buf = NULL;
        if (input_fp) {
            size_t read = fread(mpp_buffer_get_ptr(frm_buf), 1, frame_size, input_fp);
            if (read < frame_size) {
                DBG("End of input file");
                break;
            }
            DBG("Read frame %d from file", frame_id);
            use_buf = frm_buf;
        } else {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) {
                    usleep(1000);
                    continue;
                }
                perror("VIDIOC_DQBUF");
                break;
            }
            DBG("Captured frame %d from buffer %u", frame_id, buf.index);

            MppBufferInfo info;
            memset(&info, 0, sizeof(info));
            info.type = MPP_BUFFER_TYPE_DRM;
            info.fd = v4l2_bufs[buf.index].fd;
            info.size = buf.bytesused;
            if (mpp_buffer_import(&use_buf, &info)) {
                fprintf(stderr, "mpp_buffer_import failed\n");
                ioctl(v4l2_fd, VIDIOC_QBUF, &buf);
                break;
            }

            if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
                perror("VIDIOC_QBUF");
                mpp_buffer_put(use_buf);
                break;
            }
        }

        MppFrame frame = NULL;
        mpp_frame_init(&frame);
        mpp_frame_set_buffer(frame, use_buf);
        mpp_frame_set_width(frame, cfg.width);
        mpp_frame_set_height(frame, cfg.height);
        mpp_frame_set_hor_stride(frame, cfg.width);
        mpp_frame_set_ver_stride(frame, cfg.height);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);

        DBG("Feeding frame %d to encoder", frame_id);
        mpi->encode_put_frame(ctx, frame);
        mpp_frame_deinit(&frame);

        MppPacket packet = NULL;
        if (mpi->encode_get_packet(ctx, &packet) == MPP_OK && packet) {
            void *ptr = mpp_packet_get_pos(packet);
            size_t len = mpp_packet_get_length(packet);
            DBG("Encoded packet size %zu", len);
            sendto(sock, ptr, len, 0, (struct sockaddr *)&addr, sizeof(addr));

            total_bits += len * 8;
            struct timeval now;
            gettimeofday(&now, NULL);
            double elapsed =
                (now.tv_sec - start.tv_sec) + (now.tv_usec - start.tv_usec) / 1e6;
            double real_bps = elapsed > 0 ? (double)total_bits / elapsed : 0.0;
            if (log)
                fprintf(log, "frame %d size %zu set %u real %.2f\n", frame_id, len,
                        cfg.bitrate, real_bps);
            mpp_packet_deinit(&packet);
        }

        if (!input_fp && use_buf)
            mpp_buffer_put(use_buf);

        frame_id++;
        if (input_fp)
            usleep(1000000 / cfg.fps);
    }

    if (log)
        fclose(log);

    close(sock);
    if (frm_buf)
        mpp_buffer_put(frm_buf);
    if (mpi && ctx)
        mpi->reset(ctx);
    if (ctx)
        mpp_destroy(ctx);

    if (input_fp)
        fclose(input_fp);
    else if (v4l2_fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
        for (size_t i = 0; i < v4l2_bufs.size(); ++i) {
            munmap(v4l2_bufs[i].start, v4l2_bufs[i].length);
            close(v4l2_bufs[i].fd);
        }
        close(v4l2_fd);
    }

    DBG("Encoding finished after %d frames", frame_id);
    return 0;
}

