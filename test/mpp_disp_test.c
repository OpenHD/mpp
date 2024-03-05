/*
 * Copyright 2015 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(_WIN32)
#include "vld.h"
#endif

#define MODULE_TAG "pete_decode_test"

#define MULTI_BUFS

#include <string.h>
#include <stdbool.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>
#include <linux/videodev2.h>
#include <stdint.h>

#include <sys/mman.h>

#include <string.h>

#include <sys/types.h>
#include <sys/stat.h> 
#include <fcntl.h>

#include <unistd.h>

#include <sys/ioctl.h>
#include <errno.h>
#include <assert.h>

#include "rk_mpi.h"

#include "mpp_mem.h"
#include "mpp_env.h"
#include "mpp_time.h"
#include "mpp_common.h"
#include "mpi_dec_utils.h"

#define DEFAULT_PACKET_SIZE 16384

typedef struct {
    MpiDecTestCmd   *cmd;
    MppCtx          ctx;
    MppApi          *mpi;
    RK_U32          quiet;

    /* end of stream flag when set quit the loop */
    RK_U32          loop_end;

    /* input and output */
    DecBufMgr       buf_mgr;
    MppBufferGroup  frm_grp;
    MppPacket       packet;
    MppFrame        frame;

    FILE            *fp_output;
    RK_S32          frame_count;
    RK_S32          frame_num;

    RK_S64          first_pkt;
    RK_S64          first_frm;

    size_t          max_usage;
    float           frame_rate;
    RK_S64          elapsed_time;
    RK_S64          delay;
    FILE            *fp_verify;
    FrmCrc          checkcrc;
} MpiDecLoopData;

#define FRAMEBUFFERS 8

typedef struct 
{
    int drm_fd;
	drmModeRes         * __restrict drm_resources;
	drmModeConnector   * __restrict valid_connector;
	drmModeModeInfo    * __restrict chosen_resolution;
	drmModeEncoder     * __restrict screen_encoder;
    int dma_buf_fd[FRAMEBUFFERS];
    uint32_t current_crtc_id;
    drmModeCrtc * __restrict crtc_to_restore;
    uint32_t frame_buffer_id[FRAMEBUFFERS];
    uint8_t *framebuffers[FRAMEBUFFERS];
    struct drm_mode_create_dumb create_request[FRAMEBUFFERS];
    uint32_t plane_id;
    
    uint32_t decode_width;
    uint32_t decode_height;
    
    uint32_t screen_width;
    uint32_t screen_height;
} DRMInternal;

typedef struct
{
    char *data;
    size_t size;
    int eos;
} PeteReader;

int DRMCreateDumbBuffer(DRMInternal *Drm, int i, int Width, int Height);
bool DRMCreateFrameBuffer(DRMInternal *Drm, int i, int Width, int Height);
void SetFrameBuffer(DRMInternal *Drm, int BufNum);
bool DetectLEDFrame(unsigned char *Data, size_t Len);

DRMInternal DrmFull = {0};
DRMInternal *pDrm = &DrmFull;


bool InitMPP(MppApi *mpi,  MppCtx ctx);

static long long GetNowUs()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec * 1000000 + now.tv_usec;
}


MPP_RET preader_read(PeteReader *slot)
{
    static bool Configured =false;
    
    if(!Configured)
    {
        fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
        Configured = true;
        FILE *fp = fopen("/home/openhd/Header.h264", "rb");
        if(fp)
        {
            fseek(fp, 0L, SEEK_END);
            int size = ftell(fp);
            fseek(fp, 0L, SEEK_SET);
            if(size)
            {
                slot->data = malloc(size * sizeof(char));
                if(!slot->data)
                {
                    slot->size = 0;
                    slot->eos = 1;
                    return !MPP_OK;
                }
                slot->size = fread(slot->data, 1, size, fp);
                return MPP_OK;
            }
            else
            {
                printf("Header.h264 empty\n");
            }
            fclose(fp);
        }
        else
        {
            printf("Could not read Header.h264\n");
        }
    }
    #ifdef USE_FILE
    static FILE *fp = NULL;
    
    if(!fp)
    {
        fp = fopen("input.h264", "rb");
        if(!fp)
        {
            printf("Could not open input.h264 for reading\n");
            slot->data = NULL;
            slot->size = 0;
            slot->eos = 1;
            return !MPP_OK;
        }
    }
    #endif
    slot->eos = 0;
    slot->data = malloc(DEFAULT_PACKET_SIZE * sizeof(char));
    if(!slot->data)
    {
        slot->size = 0;
        slot->eos = 1;
        return !MPP_OK;
    }
    
    #ifdef USE_FILE
    slot->size = fread(slot->data, 1, DEFAULT_PACKET_SIZE, fp);
    if(feof(fp))
    {
        slot->eos = 1;
        fclose(fp);
        fp = NULL;
    }
    #else
    
    do
    {
        slot->size = fread(slot->data, 1, DEFAULT_PACKET_SIZE, stdin);
        if(slot->size == 0)
        {
            usleep(10);
        }
    } while(slot->size == 0);
    //printf("Read %d bytes\n", (unsigned int)slot->size);
    #endif
    return MPP_OK;
}

uint8_t *Bufs[FRAMEBUFFERS] = {NULL};
#ifdef MULTI_BUFS
static int ProcessFrame(MppFrame frame, MpiDecLoopData *data, int *OutFrame, bool *CovertFrame)
#else
static int ProcessFrame(MppFrame frame, MpiDecLoopData *data, uint8_t **OutFrame, bool *CovertFrame)
#endif
{
    MppApi *mpi = data->mpi;
    if (mpp_frame_get_info_change(frame)) 
    {
        RK_U32 width = mpp_frame_get_width(frame);
        RK_U32 height = mpp_frame_get_height(frame);
        RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
        RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
        RK_U32 buf_size = mpp_frame_get_buf_size(frame);
        
        pDrm->decode_width = width;
        pDrm->decode_height = height;
        
        printf("decode_get_frame get info changed found\n");
        printf("decoder require buffer w:h [%d:%d] stride [%d:%d] buf_size %d",
                  width, height, hor_stride, ver_stride, buf_size);

        int ret = mpp_buffer_group_get_external(&data->frm_grp, MPP_BUFFER_TYPE_ION);
        
        int i = 0;
        data->frame_count++;
        
        #ifdef MULTI_BUFS
        for (i=0; i<FRAMEBUFFERS; i++) 
        {
        #endif
        int DRMHandle = DRMCreateDumbBuffer(pDrm, i, width, height);
            
        if(!DRMCreateFrameBuffer(pDrm, i, width, height))
        {
            printf("Could not create DRM buffer\n");
            return -1;
        }
        
        uint8_t *framebuf;
        #ifdef MULTI_BUFS
            MppBufferInfo info = {0};
            
            info.type = MPP_BUFFER_TYPE_ION;
            info.size = buf_size ;
            info.fd = DRMHandle; 
        
            info.index = i;
        #endif
        int drm_buf_size = width * height * 3 /2;
        pDrm->framebuffers[i] = framebuf= mmap(
            0, drm_buf_size,	PROT_READ | PROT_WRITE, MAP_SHARED,
            DRMHandle, 0);
        ret = errno;
        /* Bail out if we could not map the framebuffer using this method */
        if (framebuf == NULL || framebuf == MAP_FAILED) 
        {
            printf(
                "Could not map buffer exported through PRIME : %s (%d)\n"
                "Buffer : %p\n",
                strerror(ret), ret,
                framebuf
            );
            return -1;
        }
        
        #ifdef MULTI_BUFS
            info.ptr = framebuf; // mpp_buffer_get_ptr(impl->bufs[i]);
        #else
        SetFrameBuffer(pDrm, 0);
        for (i=0; i<FRAMEBUFFERS; i++) 
        {
            
            MppBufferInfo info = {0};
            
            info.type = MPP_BUFFER_TYPE_ION;
            info.size = buf_size ;
            info.fd = DRMHandle; 
            printf("Allocating buffer of %d bytes\n", buf_size);
            info.index = i;
            //info.ptr = framebuf; // mpp_buffer_get_ptr(impl->bufs[i]);
            Bufs[i] = info.ptr = malloc(buf_size);
        #endif
            ret = mpp_buffer_commit(data->frm_grp, &info);
            assert(!ret);

        }
        /*
         * All buffer group config done. Set info change ready to let
         * decoder continue decoding
         */
        
         /* Set buffer to mpp decoder */
        ret = mpi->control(data->ctx, MPP_DEC_SET_EXT_BUF_GROUP, data->frm_grp);
        if (ret) 
        {
            printf("set buffer group failed ret %d\n", ret);
            return -1;
        }
        
        ret = mpi->control(data->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        if (ret) 
        {
            printf("%p info change ready failed ret %d\n", data->ctx, ret);
           return -1;
        }
        
        //SetFrameBuffer(pDrm, 0);
    } 
    else 
    {
        char log_buf[256];
        RK_S32 log_size = sizeof(log_buf) - 1;
        RK_S32 log_len = 0;
        RK_U32 err_info = mpp_frame_get_errinfo(frame);
        RK_U32 discard = mpp_frame_get_discard(frame);

        if (!data->first_frm)
        {
            data->first_frm = mpp_time();
        }
        
        log_len += snprintf(log_buf + log_len, log_size - log_len,
                            "decode get frame %d", data->frame_count);

        if (mpp_frame_has_meta(frame)) 
        {
            MppMeta meta = mpp_frame_get_meta(frame);
            RK_S32 temporal_id = 0;

            mpp_meta_get_s32(meta, KEY_TEMPORAL_ID, &temporal_id);

            log_len += snprintf(log_buf + log_len, log_size - log_len,
                                " tid %d", temporal_id);
        }

        if (err_info || discard) {
            log_len += snprintf(log_buf + log_len, log_size - log_len,
                                " err %x discard %x", err_info, discard);
        }
        //mpp_log_q(quiet, "%p %s\n", ctx, log_buf);
        
        data->frame_count++;
        {
            MppBuffer buffer    = NULL;
            
            
            buffer   = mpp_frame_get_buffer(frame);

            if (buffer)
            {
                MppBufferInfo info;
                int ret = mpp_buffer_info_get(buffer, &info);
                assert(!ret);
                
                
                #ifndef MULTI_BUFS
                
                *OutFrame = (RK_U8 *)mpp_buffer_get_ptr(buffer);
                #else
                 //printf("Set frame buffer %d\n", info.index);
                int index = info.index;
                
                *OutFrame = index;
                for(int CFrame = 0; CFrame < FRAMEBUFFERS; CFrame ++)
                {
                    uint32_t *fb = (uint32_t *)pDrm->framebuffers[CFrame] ;
                    fb += 512;
                    uint32_t *fb2 = fb + 512;
                    uint32_t *fb3 = fb + 2048;
                    uint32_t x = ((*fb != 0x12345678) << 3) | ((*fb2 != 0x12345678) << 2) | ((*fb3 != 0x12345678) << 1);
                    if( x)
                    {
                        //printf("Frame %d updated, index was %d, bools %x\n", CFrame, index, x);
                        *OutFrame = CFrame;
                        *fb = 0x12345678;
                        *fb2 = 0x12345678;
                        *fb3 = 0x12345678;
                    }
                }
                if(*OutFrame != index)
                {
                    *CovertFrame = true;
                }
                #endif
                
                /*
                for(int k = 0; k < 8; k ++)
                {
                    if(true == DetectLEDFrame(fbs[k], 1280*720*3/2))
                    {
                        printf("Frame %d detected LED, buf %d/%d\n", data->frame_count, k, index);
                    }
                }
                */
            }
            else
            {
                printf("Didn't get valid buffer from frame\n");
            }
        }

    }
    //frm_eos = mpp_frame_get_eos(frame);
    mpp_frame_deinit(&frame);
    return 0;
}
static int dec_simple(MpiDecLoopData *data)
{
    RK_U32 pkt_done = 0;
    MPP_RET ret = MPP_OK;
    MppCtx ctx  = data->ctx;
    MppApi *mpi = data->mpi;
    MppPacket packet = data->packet;
    PeteReader myslot, *slot = &myslot;
    int OuterLoopTimes = 0;
    static uint64_t LastLogTime = 0;
    static uint32_t DisplayedFrames = 0;
    static uint32_t DataRead = 0;
    static uint32_t Outer = 0, Inner = 0;
    static uint32_t BufferFulls = 0;
    static uint32_t CovertFrames = 0;
    
    #ifdef MULTI_BUFS
    int OutFrame = -1;
    #endif
    // when packet size is valid read the input binary file
    ret = preader_read(slot);

    mpp_assert(ret == MPP_OK);
    mpp_assert(slot);

    mpp_packet_set_data(packet, slot->data);
    mpp_packet_set_size(packet, slot->size);
    mpp_packet_set_pos(packet, slot->data);
    mpp_packet_set_length(packet, slot->size);

    do {
        Outer ++;
        int InnerLoopTimes = 0;
        RK_U32 frm_eos = 0;
        RK_S32 times = 30;
        #ifdef MULTI_BUFS
        OutFrame = -1;
        #endif
        RK_S32 get_frm = 0;
        // send the packet first if packet is not done
        if (!pkt_done) {
            ret = mpi->decode_put_packet(ctx, packet);
            //printf("Put packet\n");
            if (MPP_OK == ret) {
                pkt_done = 1;
                DataRead += slot->size;
                if (!data->first_pkt)
                    data->first_pkt = mpp_time();
            }
            else if(MPP_ERR_BUFFER_FULL == ret)
            {
                BufferFulls ++;
            }
            else
            {
                printf("decode_put_packet %d\n", ret);
            }
        }
        
        long long Start= GetNowUs();
        
        // then get all available frame and release
        do {
            Inner ++;
            MppFrame frame = NULL;
        
            get_frm = 0;
        try_again:
            ret = mpi->decode_get_frame(ctx, &frame);
            
            if (MPP_ERR_TIMEOUT == ret) {
                if (times > 0) {
                    times--;
                    msleep(1);
                    goto try_again;
                }
                mpp_err("%p decode_get_frame failed too much time\n", ctx);
            }
            if (ret) {
                mpp_err("%p decode_get_frame failed ret %d\n", ret, ctx);
                break;
            }
            
            if (frame) {
                
                bool CovertFrame = false;
                #ifndef MULTI_BUFS
                uint8_t *OutFrame = NULL;
                #endif
                if(0 == ProcessFrame(frame, data, &OutFrame, &CovertFrame))
                {
                     /* Covert frames are when mpp gives us a buffer it has decoded, but we have found 
                     * a different one it has decoded into. This can hint that it has multiple frames in the queue.
                     * in this application we don't want that so we don't display them */
                    if(!CovertFrame)
                    {
                        get_frm = 1;
                    }
                    else
                    {
                        CovertFrames ++;
                    }
                    #ifndef MULTI_BUFS
                    if(OutFrame)
                    {
                        long long End = GetNowUs();
                        // memcpy should be need here
                        DisplayedFrames ++;
                        //sprintf("Frame %d,memcpy took %dus, ret = %s\n", data->frame_count, (unsigned int)(End - Start), strerror(ret));
                    }  
                    #endif
                }
            }
            

            // try get runtime frame memory usage
            if (data->frm_grp) {
                size_t usage = mpp_buffer_group_usage(data->frm_grp);
                if (usage > data->max_usage)
                    data->max_usage = usage;
            }
            InnerLoopTimes ++;
        } while ((!get_frm) && (InnerLoopTimes <= 2));

        #ifdef MULTI_BUFS
        if(OutFrame >= 0)
        {
            SetFrameBuffer(pDrm, OutFrame);
            long long End = GetNowUs();
        
            DisplayedFrames ++;
            //printf("Frame %d, i %d memcpy took %dus, ret = %s\n", data->frame_count, OutFrame, (unsigned int)(End - Start), strerror(ret));
        }
        #endif
        
        if ((data->frame_num > 0 && (data->frame_count >= data->frame_num)) ||
            ((data->frame_num == 0) && frm_eos)) {
            data->loop_end = 1;
            break;
        }

        if (!pkt_done)
        {            
            /*
             * why sleep here:
             * mpi->decode_put_packet will failed when packet in internal queue is
             * full,waiting the package is consumed .Usually hardware decode one
             * frame which resolution is 1080p needs 2 ms,so here we sleep 1ms
             * * is enough.
             */
            msleep(1);
        }
        OuterLoopTimes ++;
    } while((!pkt_done) && (OuterLoopTimes <= 2));
    
    if((GetNowUs() - LastLogTime) > 1000000)
    {
        static uint32_t LastFramesCount = 0;
        static uint32_t TimeSinceFrameDisplayed = 0;
        uint32_t DecodedFrames = data->frame_count - LastFramesCount;
        
        printf("Decoded: %d, Displayed: %d, Data: %dKbits, BufferFulls: %d, Out/In:%d, %d\n", DecodedFrames , DisplayedFrames, DataRead / (1024 / 8), BufferFulls, Outer, Inner);
        if(0 == DecodedFrames)
        {
            TimeSinceFrameDisplayed ++;
        }
        else
        {
            TimeSinceFrameDisplayed = 0;
        }
        
        if(2 == TimeSinceFrameDisplayed)
        {
            printf("Decoder not decoding. Try a restart\n");
            TimeSinceFrameDisplayed = 0;
            
            ret = mpi->reset(ctx);
            if (ret != MPP_OK) {
                printf("failed to exec mpp->reset.\n");
                return -1;
            }
            InitMPP(mpi, ctx);
        }
        
        DisplayedFrames = 0; 
        DataRead = 0;
        BufferFulls = 0;
        CovertFrames = 0;
        LastFramesCount = data->frame_count;
        LastLogTime = GetNowUs() ;
    }

    return ret;
}


void *thread_decode(void *arg)
{
    MpiDecLoopData *data = (MpiDecLoopData *)arg;
    RK_S64 t_s, t_e;

    memset(&data->checkcrc, 0, sizeof(data->checkcrc));
    data->checkcrc.luma.sum = mpp_malloc(RK_ULONG, 512);
    data->checkcrc.chroma.sum = mpp_malloc(RK_ULONG, 512);

    t_s = mpp_time();

    while (!data->loop_end)
        dec_simple(data);

    t_e = mpp_time();
    data->elapsed_time = t_e - t_s;
    data->frame_count = data->frame_count;
    data->frame_rate = (float)data->frame_count * 1000000 / data->elapsed_time;
    data->delay = data->first_frm - data->first_pkt;

    mpp_log("decode %d frames time %lld ms delay %3d ms fps %3.2f\n",
            data->frame_count, (RK_S64)(data->elapsed_time / 1000),
            (RK_S32)(data->delay / 1000), data->frame_rate);

    MPP_FREE(data->checkcrc.luma.sum);
    MPP_FREE(data->checkcrc.chroma.sum);

    return NULL;
}

bool InitMPP(MppApi *mpi,  MppCtx ctx)
{
    
    // config for runtime mode
    MppDecCfg cfg       = NULL;
    RK_U32 need_split   = 1;
    
    int fast_mode = 1;
    
    mpp_dec_cfg_init(&cfg);

    /* get default config from decoder context */
    int ret = mpi->control(ctx, MPP_DEC_GET_CFG, cfg);
    if (ret) {
        mpp_err("%p failed to get decoder cfg ret %d\n", ctx, ret);
        return false;
    }

    /*
     * split_parse is to enable mpp internal frame spliter when the input
     * packet is not aplited into frames.
     */
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret) {
        mpp_err("%p failed to set split_parse ret %d\n", ctx, ret);
        return false;
    }

    RK_U32 dat = 0xffff;
    RK_U32 ret1 = mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &dat);
     dat = 0xffff;
    RK_U32 ret2 = mpi->control(ctx, MPP_DEC_SET_DISABLE_ERROR, &dat);
     dat = 0xffff;
    RK_U32 ret3 = mpi->control(ctx, MPP_DEC_SET_IMMEDIATE_OUT, &dat);
    dat = 0xffff;
    RK_U32 ret4 = mpi->control(ctx, MPP_DEC_SET_ENABLE_FAST_PLAY, &dat);
    if(ret1 | ret2 | ret3 |ret4)
    {
        printf("Could not set decoder params on startup\n");
        return false;
    }
    ret = mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        mpp_err("%p failed to set cfg %p ret %d\n", ctx, cfg, ret);
        return false;
    }
  mpi->control (ctx, MPP_DEC_SET_PARSER_FAST_MODE,
        &fast_mode);
    return true;
}

int dec_decode(MpiDecTestCmd *cmd)
{
    // base flow context
    MppCtx ctx          = NULL;
    MppApi *mpi         = NULL;

    // input / output
    MppPacket packet    = NULL;
    MppFrame  frame     = NULL;

    // paramter for resource malloc
    RK_U32 width        = cmd->width;
    RK_U32 height       = cmd->height;
    MppCodingType type  = cmd->type;


    // resources
    MppBuffer frm_buf   = NULL;
    pthread_t thd;
    pthread_attr_t attr;
    MpiDecLoopData data;
    MPP_RET ret = MPP_OK;

    mpp_log("mpi_dec_test start\n");
    memset(&data, 0, sizeof(data));
    pthread_attr_init(&attr);

    cmd->simple = (cmd->type != MPP_VIDEO_CodingMJPEG) ? (1) : (0);

    ret = dec_buf_mgr_init(&data.buf_mgr);
    if (ret) {
        mpp_err("dec_buf_mgr_init failed\n");
        goto MPP_TEST_OUT;
    }

    if (cmd->simple) {
        ret = mpp_packet_init(&packet, NULL, 0);
        if (ret) {
            mpp_err("mpp_packet_init failed\n");
            goto MPP_TEST_OUT;
        }
    } 

    // decoder demo
    ret = mpp_create(&ctx, &mpi);
    if (ret) {
        mpp_err("mpp_create failed\n");
        goto MPP_TEST_OUT;
    }

    mpp_log("%p mpi_dec_test decoder test start w %d h %d type %d\n",
            ctx, width, height, type);
    int fast_mode = 1;
    mpi->control (ctx, MPP_DEC_SET_PARSER_FAST_MODE,
        &fast_mode);
    int immediate = 1;
    mpi->control (ctx, MPP_DEC_SET_IMMEDIATE_OUT,
        &immediate);
    
    ret = mpp_init(ctx, MPP_CTX_DEC, type);
    if (ret) {
        mpp_err("%p mpp_init failed\n", ctx);
        goto MPP_TEST_OUT;
    }
    if(false == InitMPP(mpi, ctx))
    {
        goto MPP_TEST_OUT;
    }
    
    data.cmd            = cmd;
    data.ctx            = ctx;
    data.mpi            = mpi;
    data.loop_end       = 0;
    data.packet         = packet;
    data.frame          = frame;
    data.frame_count    = 8;
    data.frame_num      = cmd->frame_num;
    data.quiet          = cmd->quiet;

    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    ret = pthread_create(&thd, &attr, thread_decode, &data);
    if (ret) {
        mpp_err("failed to create thread for input ret %d\n", ret);
        goto MPP_TEST_OUT;
    }

    pthread_join(thd, NULL);

    cmd->max_usage = data.max_usage;

    ret = mpi->reset(ctx);
    if (ret) {
        mpp_err("%p mpi->reset failed\n", ctx);
        goto MPP_TEST_OUT;
    }

MPP_TEST_OUT:
    if (data.packet) {
        mpp_packet_deinit(&data.packet);
        data.packet = NULL;
    }

    if (frame) {
        mpp_frame_deinit(&frame);
        frame = NULL;
    }

    if (ctx) {
        mpp_destroy(ctx);
        ctx = NULL;
    }

    if (!cmd->simple) {
        if (frm_buf) {
            mpp_buffer_put(frm_buf);
            frm_buf = NULL;
        }
    }

    data.frm_grp = NULL;
    if (data.buf_mgr) {
        dec_buf_mgr_deinit(data.buf_mgr);
        data.buf_mgr = NULL;
    }
/*
    if (cfg) {
        mpp_dec_cfg_deinit(cfg);
        cfg = NULL;
    }
*/
    pthread_attr_destroy(&attr);

    return ret;
}

bool OpenDRM(DRMInternal *Drm)
{
    
    /* DRM is based on the fact that you can connect multiple screens,
	 * on multiple different connectors which have, of course, multiple
	 * encoders that transform CRTC (The screen final buffer where all
	 * the framebuffers are blended together) represented in XRGB8888 (or
	 * similar) into something the selected screen comprehend.
	 * (Think XRGB8888 to DVI-D format for example)
	 * 
	 * The default selection system is simple :
	 * - We try to find the first connected screen and choose its
	 *   preferred resolution.
	 */

	/* Open the DRM device node and get a File Descriptor */
	Drm->drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

	if (Drm->drm_fd < 0) {
		printf("Could not open /dev/dri/card0 : %m\n");
		return false;
	}

	/* Let's see what we can use through this drm node */
	Drm->drm_resources = drmModeGetResources(Drm->drm_fd);

	/* Get a valid connector. A valid connector is one that's connected */
	for (int_fast32_t c = 0; c < Drm->drm_resources->count_connectors; c++) {
		Drm->valid_connector =
			drmModeGetConnector(Drm->drm_fd, Drm->drm_resources->connectors[c]);

		if (Drm->valid_connector->connection == DRM_MODE_CONNECTED)
			break;

		drmModeFreeConnector(Drm->valid_connector);
		Drm->valid_connector = NULL;
	}

	/* Bail out if nothing was connected */
	if (!Drm->valid_connector) {
		/* Then there was no connectors,
		 * or no connector were connected */
		printf("No connectors or no connected connectors found...\n");
		return false;
	}

	/* Get the preferred resolution */
	for (int_fast32_t m = 0; m < Drm->valid_connector->count_modes; m++) {
		drmModeModeInfo * __restrict tested_resolution =
			&Drm->valid_connector->modes[m];
		if (tested_resolution->type & DRM_MODE_TYPE_PREFERRED) {
			Drm->chosen_resolution = tested_resolution;
			break;
		}
	}

	/* Bail out if there's no such thing as a "preferred resolution" */
	if (!Drm->chosen_resolution) {
		printf(
			"No preferred resolution on the selected connector %u ?\n",
			Drm->valid_connector->connector_id
		);
		return false;
	}
    Drm->screen_width = Drm->chosen_resolution->hdisplay;
    Drm->screen_height = Drm->chosen_resolution->vdisplay;
    printf("Screen resolution: %d x %d\n", Drm->screen_width, Drm->screen_height);
    
	/* Get an encoder that will transform our CRTC data into something
	 * the screen comprehend natively, through the chosen connector */
	Drm->screen_encoder =
		drmModeGetEncoder(Drm->drm_fd, Drm->valid_connector->encoder_id);
	
	printf(
			"Using mode %s on connector %u.\n",
			Drm->chosen_resolution->name,
			Drm->valid_connector->connector_id
		);
	/* If there's no screen encoder through the chosen connector, bail
	 * out quickly. */
	if (!Drm->screen_encoder) {
		printf(
			"Could not retrieve the encoder for mode %s on connector %u.\n",
			Drm->chosen_resolution->name,
			Drm->valid_connector->connector_id
		);
		return false;
	}

    uint32_t plane_id = 0;

    drmModePlaneRes *plane_res = drmModeGetPlaneResources(Drm->drm_fd);

    for (unsigned int i = 0; (i < plane_res->count_planes) && (plane_id == 0); i++) {
        drmModePlane *plane = drmModeGetPlane(Drm->drm_fd, plane_res->planes[i]);
        drmModeObjectProperties *props = drmModeObjectGetProperties(Drm->drm_fd, plane->plane_id, DRM_MODE_OBJECT_ANY);

        for (unsigned int j = 0; (j < props->count_props) && (plane_id == 0); j++) {
            drmModePropertyRes *prop = drmModeGetProperty(Drm->drm_fd, props->props[j]);

            if ((strcmp(prop->name, "type") == 0) && (props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY)) {
                plane_id = plane->plane_id;
            }

            drmModeFreeProperty(prop);
        }

        drmModeFreeObjectProperties(props);
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(plane_res);
    Drm->plane_id = plane_id;

    return true;
}


int DRMCreateDumbBuffer(DRMInternal *Drm, int i, int Width, int Height)
{

    /* Request a dumb buffer */
    Drm->create_request[i].width  = Width;
    Drm->create_request[i].height = Height;
    Drm->create_request[i].bpp    = 12;
    int ret = ioctl(Drm->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &Drm->create_request[i]);

    /* Bail out if we could not allocate a dumb buffer */
    if (ret) {
        printf(
            "Dumb Buffer Object Allocation request of %ux%u@%u failed : %s\n",
            Drm->create_request[i].width, Drm->create_request[i].height,
            Drm->create_request[i].bpp,
            strerror(ret)
        );
        return 0;
    }
    
    
    /* For this test only : Export our dumb buffer using PRIME */
    /* This will provide us a PRIME File Descriptor that we'll use to
     * map the represented buffer. This could be also be used to reimport
     * the GEM buffer into another GPU */
    struct drm_prime_handle prime_request = {
        .handle = Drm->create_request[i].handle,
        .flags  = DRM_CLOEXEC | DRM_RDWR,
        .fd     = -1
    };

    ret = ioctl(Drm->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_request);
    Drm->dma_buf_fd[i] = prime_request.fd;

    /* If we could not export the buffer, bail out since that's the
     * purpose of our test */
    if (ret || Drm->dma_buf_fd[i] < 0) {
        printf(
            "Could not export buffer : %s (%d) - FD : %d\n",
            strerror(ret), ret,
            Drm->dma_buf_fd[i]
        );
        return 0;
    }
    return prime_request.fd;
}

bool DRMCreateFrameBuffer(DRMInternal *Drm, int i, int Width, int Height)
{
    /* Create a framebuffer */
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    int modes;
    
    //modes = DRM_FORMAT_NV12;
    modes = DRM_FORMAT_NV12;
    pitches[0] = Width;
    handles[1] = Drm->create_request[i].handle;
    pitches[1] = Width;
    offsets[1] = Width * Height;
    handles[0] = Drm->create_request[i].handle;
    offsets[0] = 0;
    int ret = drmModeAddFB2(Drm->drm_fd, Width, Height,
                modes, handles, pitches, offsets, &Drm->frame_buffer_id[i], 0);
    
    /* Without framebuffer, we won't do anything so bail out ! */
    if (ret) {
        printf(
            "Could not add a framebuffer using drmModeAddFB : %s\n",
            strerror(ret)
        );
        return false;
    }


	return true;
}

bool SetCRC(DRMInternal *Drm)
{
	/* We assume that the currently chosen encoder CRTC ID is the current
	 * one.
	 */
	Drm->current_crtc_id = Drm->screen_encoder->crtc_id;
    printf("crtc_id=%d\n", Drm->current_crtc_id);
	if (!Drm->current_crtc_id) {
		printf("The retrieved encoder has no CRTC attached... ?\n");
        return false;
	}

	/* Backup the informations of the CRTC to restore when we're done.
	 * The most important piece seems to currently be the buffer ID.
	 */
	Drm->crtc_to_restore =
		drmModeGetCrtc(Drm->drm_fd, Drm->current_crtc_id);

	if (!Drm->crtc_to_restore) {
		printf("Could not retrieve the current CRTC with a valid ID !\n");
        return false;
	}

	/* Set the CRTC so that it uses our new framebuffer */
	drmModeSetCrtc(
		Drm->drm_fd, Drm->current_crtc_id, Drm->frame_buffer_id[0],
		0, 0,
		&Drm->valid_connector->connector_id,
		1,
		Drm->chosen_resolution);
    return true;
}

void SetFrameBuffer(DRMInternal *Drm, int BufNum)
{
    if(BufNum < FRAMEBUFFERS)
    {
        drmModeSetCrtc(
            Drm->drm_fd, Drm->current_crtc_id, Drm->frame_buffer_id[BufNum],
            0, 0,
            &Drm->valid_connector->connector_id,
            1,
            Drm->chosen_resolution);
    }
    else
    {
        printf("SetFrameBuffer invalid buffer %d\n", BufNum);
    }
}

void CloseFrameBuffer(DRMInternal *Drm)
{
#if 0
	//munmap(Drm->framebuffer, Drm->create_request.size);

	// very ugly but will do for an example...
	{
		struct drm_mode_destroy_dumb destroy_request = {
			.handle = Drm->create_request.handle
		};
		
		ioctl(Drm->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_request);
	}
    #endif
	drmModeSetCrtc(
		Drm->drm_fd,
		Drm->crtc_to_restore->crtc_id, Drm->crtc_to_restore->buffer_id,
		0, 0, &Drm->valid_connector->connector_id, 1, &Drm->crtc_to_restore->mode
	);

	drmModeRmFB(Drm->drm_fd, Drm->frame_buffer_id[0]);
	drmModeFreeEncoder(Drm->screen_encoder);
    
}

void CloseDRM(DRMInternal *Drm)
{
	drmModeFreeModeInfo(Drm->chosen_resolution);
	drmModeFreeConnector(Drm->valid_connector);
	close(Drm->drm_fd);
}

int main(void)
{
    RK_S32 ret = 0;
    MpiDecTestCmd  cmd_ctx;
    MpiDecTestCmd* cmd = &cmd_ctx;


    memset((void*)cmd, 0, sizeof(*cmd));
    cmd->format = MPP_FMT_BUTT;
    cmd->pkt_size = MPI_DEC_STREAM_SIZE;
    cmd->buf_mode = MPP_DEC_BUF_EXTERNAL;
    strcpy(cmd->file_output, "output.raw");
    //cmd->have_output = 1;
    cmd->type = 7;
    
    mpi_dec_test_cmd_options(cmd);
mpp_set_log_level(MPP_LOG_DEBUG);
    printf("Buf mode = %d\n", cmd->buf_mode);
    
    printf("Decode set up, now opening screen\n");
    
    if(!OpenDRM(pDrm))
    {
        printf("Could not open screen using DRM\n");
        return 0;
    }
    if(!SetCRC(pDrm))
    {
        return 0;
    }
    
    ret = dec_decode(cmd);
    if (MPP_OK == ret)
        mpp_log("test success max memory %.2f MB\n", cmd->max_usage / (float)(1 << 20));
    else
        mpp_err("test failed ret %d\n", ret);

    
    mpi_dec_test_cmd_deinit(cmd);

    return ret;
}

#define WIDTH 960
#define HEIGHT 544
bool DetectLEDFrame(unsigned char *Data, size_t Len)
{
    static int Count = 0;
    uint64_t TotLum = 0;
    uint32_t AvLum;
    int WindowSize = 8; // -WindowSize to +WindowSize
    int StartY = (HEIGHT) - WindowSize; // Changed from HEIGHT / 2 to give best possible results
    int StartX = (WIDTH / 2) - WindowSize;

    /* Make sure we can't retrigger within 0.16s */
    if(Count <= 10)
    {
        Count ++;
        return false;
    }
    for(int y = StartY; y < (StartY + WindowSize); y ++)
    {
        for(int x = StartX; x < (StartX + WindowSize); x ++)
        {
            unsigned int Elem = y * WIDTH + x;
            if(Elem >= (unsigned int)Len)
            {
                printf("DetectLEDFrame() Invalid Elem %d, y=%d, x=%d\n", Elem, y, x);
            }
            else
            {
               TotLum += *(Data + Elem);
            }
        }
    }
    AvLum = TotLum / (WindowSize * WindowSize);
   
    //printf("AvLum=%d\n", AvLum);
    if(AvLum > 128)
    {
        Count = 0;
        
        /* Optionally add a small white & black rectangle to the top left of the frame */
        for(int y = 0; y<12; y ++)
        {
            for(int x = 0; x < 12; x += 2)
            {
                Data[y * WIDTH + x] = 0;
                Data[y * WIDTH + x + 1] = 255;
            }
        }
        return true;
    }
    return false;
}



