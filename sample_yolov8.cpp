#define LOG_TAG "SampleOD"
#define LOG_LEVEL LOG_LEVEL_INFO

#ifdef __cplusplus
extern "C" {
#endif

#include "middleware_utils.h"

#ifdef __cplusplus
}
#endif

#include "sample_utils.h"
#include "vi_vo_utils.h"

#include <cvi_comm.h>
#include <sample_comm.h>
#include "cvi_tdl.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <iostream>

#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include "cvi_buffer.h"
#include "cvi_ae_comm.h"
#include "cvi_awb_comm.h"
#include "cvi_comm_isp.h"
#include "cvi_comm_sns.h"
#include "cvi_ae.h"
#include "cvi_awb.h"
#include "cvi_isp.h"
#include "cvi_sns_ctrl.h"
#include "cvi_sys.h"

#include <queue>

#include <termios.h>
#include <time.h>

using namespace std;

int x = 560;
int y = 140;
int w = 800;
int h = 800;

static VPSS_GRP g_cropVpssGrp = -1;
static const VPSS_CHN g_cropVpssChn = 0;

static volatile bool bExit = false;
static SAMPLE_VI_CONFIG_S g_stViConfig;
static SAMPLE_INI_CFG_S g_stIniCfg;

static std::queue<VIDEO_FRAME_INFO_S> g_frame_queue;
static pthread_mutex_t g_queue_mutex;
static pthread_cond_t g_queue_cond_not_empty;
static pthread_cond_t g_queue_cond_not_full;
static const int MAX_QUEUE_SIZE = 2;

static volatile bool g_save_frame_flag = false;
static struct termios old_tio, new_tio;

static void set_terminal_mode() {
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= (~ICANON & ~ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

static void restore_terminal_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
}

static void save_frame_to_yuv(const VIDEO_FRAME_INFO_S *stFrame) {
    if (!stFrame) {
        printf("Error: Frame is null. Cannot save.\n");
        return;
    }
    char filename[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(filename, sizeof(filename)-1, "frame_%Y%m%d_%H%M%S.yuv", t);
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("Error: Failed to open file %s for writing.\n", filename);
        return;
    }
    CVI_U32 u32OffsetY = stFrame->stVFrame.s16OffsetTop;
    CVI_U32 u32OffsetX = stFrame->stVFrame.s16OffsetLeft;
    CVI_U32 u32EffectiveWidth = stFrame->stVFrame.u32Width - stFrame->stVFrame.s16OffsetLeft - stFrame->stVFrame.s16OffsetRight;
    CVI_U32 u32EffectiveHeight = stFrame->stVFrame.u32Height - stFrame->stVFrame.s16OffsetTop - stFrame->stVFrame.s16OffsetBottom;
    printf("\nSaving frame to %s (format: %d, effective size: %ux%u, buffer size: %ux%u)\n",
           filename, stFrame->stVFrame.enPixelFormat,
           u32EffectiveWidth, u32EffectiveHeight,
           stFrame->stVFrame.u32Width, stFrame->stVFrame.u32Height);
    printf("Offsets (L,T): (%u, %u), Strides: [%u, %u, %u]\n",
           u32OffsetX, u32OffsetY,
           stFrame->stVFrame.u32Stride[0], stFrame->stVFrame.u32Stride[1], stFrame->stVFrame.u32Stride[2]);
    CVI_U32 u32TotalSize = stFrame->stVFrame.u32Length[0] + stFrame->stVFrame.u32Length[1] + stFrame->stVFrame.u32Length[2];
    void *pVirAddr = NULL;
    if (stFrame->stVFrame.u64PhyAddr[0] != 0 && u32TotalSize > 0) {
        pVirAddr = CVI_SYS_Mmap(stFrame->stVFrame.u64PhyAddr[0], u32TotalSize);
    }
    if (pVirAddr == NULL) {
        printf("Error: CVI_SYS_Mmap failed.\n");
        fclose(fp);
        return;
    }
    CVI_SYS_IonInvalidateCache(stFrame->stVFrame.u64PhyAddr[0], pVirAddr, u32TotalSize);
    CVI_U8 *pDataY = (CVI_U8 *)pVirAddr;
    CVI_U32 u32StrideY = stFrame->stVFrame.u32Stride[0];
    CVI_U8 *pStartOffsetY = pDataY + (u32OffsetY * u32StrideY) + u32OffsetX;
    printf("Writing Y plane...\n");
    for (CVI_U32 y = 0; y < u32EffectiveHeight; ++y) {
        fwrite(pStartOffsetY, 1, u32EffectiveWidth, fp);
        pStartOffsetY += u32StrideY;
    }
    if (stFrame->stVFrame.u32Length[1] > 0) {
        CVI_U8 *pDataVU = pDataY + stFrame->stVFrame.u32Length[0];
        CVI_U32 u32StrideVU = stFrame->stVFrame.u32Stride[1];
        CVI_U32 u32UVHeight = u32EffectiveHeight / 2;
        CVI_U32 u32UVOffsetY = u32OffsetY / 2;
        CVI_U32 u32UVOffsetXBytes = u32OffsetX & ~1;
        CVI_U32 u32UVWidthBytes = u32EffectiveWidth;
        CVI_U8 *pStartOffsetVU = pDataVU + (u32UVOffsetY * u32StrideVU) + u32UVOffsetXBytes;
        printf("Writing VU plane (NV21)...\n");
        for (CVI_U32 y = 0; y < u32UVHeight; ++y) {
            fwrite(pStartOffsetVU, 1, u32UVWidthBytes, fp);
            pStartOffsetVU += u32StrideVU;
        }
    }
    CVI_SYS_Munmap(pVirAddr, u32TotalSize);
    fflush(fp);
    fclose(fp);
    printf("Frame saved successfully to %s.\n", filename);
}

typedef struct {
    cvitdl_handle_t stTDLHandle;
} SAMPLE_TDL_TDL_THREAD_ARG_S;

static int sys_vi_init(void)
{
    MMF_VERSION_S stVersion;
    SAMPLE_INI_CFG_S   stIniCfg;
    SAMPLE_VI_CONFIG_S stViConfig;
    PIC_SIZE_E enPicSize;
    SIZE_S stSize;
    CVI_S32 s32Ret = CVI_SUCCESS;
    LOG_LEVEL_CONF_S log_conf;

    CVI_SYS_GetVersion(&stVersion);
    SAMPLE_PRT("MMF Version:%s\n", stVersion.version);
    log_conf.enModId = CVI_ID_LOG;
    log_conf.s32Level = CVI_DBG_INFO;
    CVI_LOG_SetLevelConf(&log_conf);

    if (SAMPLE_COMM_VI_ParseIni(&stIniCfg)) {
        SAMPLE_PRT("Parse complete\n");
    }
    CVI_VI_SetDevNum(stIniCfg.devNum);
    s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
    if (s32Ret != CVI_SUCCESS)
        return s32Ret;

    memcpy(&g_stViConfig, &stViConfig, sizeof(SAMPLE_VI_CONFIG_S));
    memcpy(&g_stIniCfg, &stIniCfg, sizeof(SAMPLE_INI_CFG_S));

    s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(stIniCfg.enSnsType[0], &enPicSize);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("SAMPLE_COMM_VI_GetSizeBySensor failed with %#x\n", s32Ret);
        return s32Ret;
    }

    s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed with %#x\n", s32Ret);
        return s32Ret;
    }

    s32Ret = SAMPLE_PLAT_SYS_INIT(stSize);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("sys init failed. s32Ret: 0x%x !\n", s32Ret);
        return s32Ret;
    }

    s32Ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("vi init failed. s32Ret: 0x%x !\n", s32Ret);
        return s32Ret;
    }

    printf("Setting up VPSS group for cropping...\n");
    g_cropVpssGrp = 0;
    VPSS_GRP_ATTR_S    stVpssGrpAttr;
    VPSS_CHN_ATTR_S    stVpssChnAttr;
    VPSS_CROP_INFO_S   stCropInfo;

    memset(&stVpssGrpAttr, 0, sizeof(stVpssGrpAttr));
    stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stVpssGrpAttr.enPixelFormat = stViConfig.astViInfo[0].stChnInfo.enPixFormat;
    stVpssGrpAttr.u32MaxW = stSize.u32Width;
    stVpssGrpAttr.u32MaxH = stSize.u32Height;
    stVpssGrpAttr.u8VpssDev = 0;

    s32Ret = CVI_VPSS_CreateGrp(g_cropVpssGrp, &stVpssGrpAttr);
    if (s32Ret != CVI_SUCCESS) {
        printf("CVI_VPSS_CreateGrp(grp:%d) failed with %#x!\n", g_cropVpssGrp, s32Ret);
        return s32Ret;
    }

    memset(&stCropInfo, 0, sizeof(stCropInfo));
    stCropInfo.bEnable = CVI_TRUE;
    stCropInfo.enCropCoordinate = VPSS_CROP_ABS_COOR;
    stCropInfo.stCropRect.s32X = x;
    stCropInfo.stCropRect.s32Y = y;
    stCropInfo.stCropRect.u32Width  = w;
    stCropInfo.stCropRect.u32Height = h;
    s32Ret = CVI_VPSS_SetGrpCrop(g_cropVpssGrp, &stCropInfo);
    if (s32Ret != CVI_SUCCESS) {
        printf("CVI_VPSS_SetGrpCrop failed with %#x\n", s32Ret);
        return s32Ret;
    }
    printf("VPSS group %d crop set to x=%d y=%d w=%d h=%d\n", g_cropVpssGrp, x, y, w, h);

    memset(&stVpssChnAttr, 0, sizeof(stVpssChnAttr));
    stVpssChnAttr.u32Width                  = w;
    stVpssChnAttr.u32Height                 = h;
    stVpssChnAttr.enVideoFormat             = VIDEO_FORMAT_LINEAR;
    stVpssChnAttr.enPixelFormat             = stVpssGrpAttr.enPixelFormat;
    stVpssChnAttr.stFrameRate.s32SrcFrameRate = -1;
    stVpssChnAttr.stFrameRate.s32DstFrameRate = -1;
    stVpssChnAttr.u32Depth                  = 1;
    stVpssChnAttr.bMirror                   = CVI_FALSE;
    stVpssChnAttr.bFlip                     = CVI_FALSE;
    stVpssChnAttr.stNormalize.bEnable       = CVI_FALSE;

    s32Ret = CVI_VPSS_SetChnAttr(g_cropVpssGrp, g_cropVpssChn, &stVpssChnAttr);
    if (s32Ret != CVI_SUCCESS) {
        printf("CVI_VPSS_SetChnAttr failed with %#x\n", s32Ret);
        return s32Ret;
    }

    s32Ret = CVI_VPSS_EnableChn(g_cropVpssGrp, g_cropVpssChn);
    if (s32Ret != CVI_SUCCESS) {
        printf("CVI_VPSS_EnableChn failed with %#x\n", s32Ret);
        return s32Ret;
    }

    s32Ret = CVI_VPSS_StartGrp(g_cropVpssGrp);
    if (s32Ret != CVI_SUCCESS) {
        printf("CVI_VPSS_StartGrp failed with %#x\n", s32Ret);
        return s32Ret;
    }

    return CVI_SUCCESS;
}

static void sys_vi_deinit(void)
{
    if (g_cropVpssGrp != -1) {
        CVI_VPSS_StopGrp(g_cropVpssGrp);
        CVI_VPSS_DestroyGrp(g_cropVpssGrp);
        g_cropVpssGrp = -1;
    }

    SAMPLE_COMM_VI_DestroyIsp(&g_stViConfig);
    SAMPLE_COMM_VI_DestroyVi(&g_stViConfig);
    SAMPLE_COMM_SYS_Exit();
}


static CVI_S32 init_yolo_param(const cvitdl_handle_t tdl_handle) {
    printf("Setting up YOLOv8 parameters...\n");
    YoloPreParam preprocess_cfg = CVI_TDL_Get_YOLO_Preparam(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);
    for (int i = 0; i < 3; i++) {
        preprocess_cfg.factor[i] = 0.003922;
        preprocess_cfg.mean[i] = 0.0;
    }
    preprocess_cfg.format = PIXEL_FORMAT_RGB_888_PLANAR;
    preprocess_cfg.rescale_type = RESCALE_CENTER;
    CVI_S32 ret = CVI_TDL_Set_YOLO_Preparam(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, preprocess_cfg);
    if (ret != CVI_SUCCESS) {
        fprintf(stderr, "Failed to set YOLOv8 preprocess parameters, error: %#x\n", ret);
        return ret;
    }
    YoloAlgParam yolov8_param = CVI_TDL_Get_YOLO_Algparam(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);
    yolov8_param.cls = 2;
    ret = CVI_TDL_Set_YOLO_Algparam(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, yolov8_param);
    if (ret != CVI_SUCCESS) {
        fprintf(stderr, "Failed to set YOLOv8 algorithm parameters, error: %#x\n", ret);
        return ret;
    }
    CVI_TDL_SetModelThreshold(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, 0.5);
    CVI_TDL_SetModelNmsThreshold(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, 0.5);
    printf("YOLOv8 parameters setup successfully.\n");
    return CVI_SUCCESS;
}

void *get_frame_thread(void *args) {
    printf("Enter GetFrame thread (Producer)\n");
    VIDEO_FRAME_INFO_S stRawFrame, stCroppedFrame;
    CVI_S32 s32Ret;

    while (bExit == false) {
        s32Ret = CVI_VI_GetChnFrame(0, 0, &stRawFrame, 2000);
        if (s32Ret != CVI_SUCCESS) {
            if (bExit) break;
            printf("CVI_VI_GetChnFrame failed with %#x in get_frame_thread\n", s32Ret);
            usleep(100 * 1000);
            continue;
        }

        s32Ret = CVI_VPSS_SendFrame(g_cropVpssGrp, &stRawFrame, 1000);
        if (s32Ret != CVI_SUCCESS) {
            printf("CVI_VPSS_SendFrame failed with %#x\n", s32Ret);
            CVI_VI_ReleaseChnFrame(0, 0, &stRawFrame);
            continue;
        }

        CVI_VI_ReleaseChnFrame(0, 0, &stRawFrame);

        s32Ret = CVI_VPSS_GetChnFrame(g_cropVpssGrp, g_cropVpssChn, &stCroppedFrame, 1000);
        if (s32Ret != CVI_SUCCESS) {
            printf("CVI_VPSS_GetChnFrame failed with %#x\n", s32Ret);
            continue;
        }

        pthread_mutex_lock(&g_queue_mutex);
        while (g_frame_queue.size() >= MAX_QUEUE_SIZE && bExit == false) {
            pthread_cond_wait(&g_queue_cond_not_full, &g_queue_mutex);
        }
        if (bExit == true) {
            pthread_mutex_unlock(&g_queue_mutex);
            CVI_VPSS_ReleaseChnFrame(g_cropVpssGrp, g_cropVpssChn, &stCroppedFrame);
            break;
        }
        g_frame_queue.push(stCroppedFrame);
        pthread_cond_signal(&g_queue_cond_not_empty);
        pthread_mutex_unlock(&g_queue_mutex);
    }

    printf("Exit GetFrame thread\n");
    pthread_exit(NULL);
}

void *run_tdl_thread(void *args) {
    printf("Enter TDL thread (Consumer)\n");
    SAMPLE_TDL_TDL_THREAD_ARG_S *pstTDLArgs = (SAMPLE_TDL_TDL_THREAD_ARG_S *)args;
    VIDEO_FRAME_INFO_S stFrame;
    cvtdl_object_t stObjMeta = {0};
    CVI_S32 s32Ret;

    long long ll_frame_count = 0;
    struct timeval start_time, end_time, fps_start_time;
    double inference_time;
    int frame_batch_count = 0;

    gettimeofday(&fps_start_time, NULL);

    while (bExit == false) {
        pthread_mutex_lock(&g_queue_mutex);
        while (g_frame_queue.empty() && bExit == false) {
            pthread_cond_wait(&g_queue_cond_not_empty, &g_queue_mutex);
        }
        if (bExit == true) {
            pthread_mutex_unlock(&g_queue_mutex);
            break;
        }
        stFrame = g_frame_queue.front();
        g_frame_queue.pop();
        pthread_cond_signal(&g_queue_cond_not_full);
        pthread_mutex_unlock(&g_queue_mutex);

        if (g_save_frame_flag) {
            save_frame_to_yuv(&stFrame);
            g_save_frame_flag = false;
        }

        gettimeofday(&start_time, NULL);
        s32Ret = CVI_TDL_YOLOV8_Detection(pstTDLArgs->stTDLHandle, &stFrame, &stObjMeta);
        gettimeofday(&end_time, NULL);
        inference_time = (end_time.tv_sec - start_time.tv_sec) +
                         (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
        
        CVI_VPSS_ReleaseChnFrame(g_cropVpssGrp, g_cropVpssChn, &stFrame);

        if (s32Ret != CVI_TDL_SUCCESS) {
            printf("Inference failed with ret=%x\n", s32Ret);
            CVI_TDL_Free(&stObjMeta);
            continue;
        }

        ll_frame_count++;
        frame_batch_count++;
        printf("\rFrame %lld | Inference %.5f s | Detected %u Object(s)",
               ll_frame_count, inference_time, stObjMeta.size);
        fflush(stdout);

        if (frame_batch_count >= 500) {
            gettimeofday(&end_time, NULL);
            double elapsed_s = (end_time.tv_sec - fps_start_time.tv_sec) +
                               (end_time.tv_usec - fps_start_time.tv_usec) / 1000000.0;
            if (elapsed_s > 0) {
                float fps = frame_batch_count / elapsed_s;
                printf("\n-------------------- [ FPS: %.2f ] --------------------\n", fps);
            }
            frame_batch_count = 0;
            gettimeofday(&fps_start_time, NULL);
        }
        CVI_TDL_Free(&stObjMeta);
    }

    pthread_mutex_lock(&g_queue_mutex);
    while (!g_frame_queue.empty()) {
        stFrame = g_frame_queue.front();
        g_frame_queue.pop();
        CVI_VPSS_ReleaseChnFrame(g_cropVpssGrp, g_cropVpssChn, &stFrame);
    }
    pthread_mutex_unlock(&g_queue_mutex);

    if (pstTDLArgs->stTDLHandle) {
        CVI_TDL_DestroyHandle(pstTDLArgs->stTDLHandle);
        pstTDLArgs->stTDLHandle = NULL;
    }

    printf("\nExit TDL thread\n");
    pthread_exit(NULL);
}

static void SampleHandleSig(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        bExit = true;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2 && argc != 3) {
        printf("sample_vi_od - A simple object detection sample\n\n");
        printf("Usage: %s <MODEL_PATH> [THRESHOLD]\n\n", argv[0]);
        printf("   MODEL_PATH: Path to the cvimodel file.\n");
        printf("   THRESHOLD: (Optional) Detection threshold, e.g., 0.5.\n\n");
        printf("While running, press 's' then Enter to save the current frame as a .yuv file.\n");
        return CVI_FAILURE;
    }

    CVI_S32 s32Ret = CVI_SUCCESS;
    cvitdl_handle_t stTDLHandle = NULL;
    SAMPLE_TDL_TDL_THREAD_ARG_S ai_args;
    pthread_t stFrameThread, stTDLThread;

    memset(&ai_args, 0, sizeof(ai_args));

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SampleHandleSig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    s32Ret = sys_vi_init();
    if (s32Ret != CVI_SUCCESS) {
        printf("Sys VI init failed with ret=%#x\n", s32Ret);
        return -1;
    }

    s32Ret = CVI_TDL_CreateHandle(&stTDLHandle);
    if (s32Ret != CVI_SUCCESS) {
        fprintf(stderr, "CVI_TDL_CreateHandle failed with %#x!\n", s32Ret);
        goto create_tdl_fail;
    }
    CVI_TDL_SetVpssTimeout(stTDLHandle, 1000);

    s32Ret = init_yolo_param(stTDLHandle);
    if (s32Ret != CVI_SUCCESS) {
        printf("init_yolo_param failed!\n");
        goto setup_tdl_fail;
    }

    printf("Opening model: %s\n", argv[1]);
    s32Ret = CVI_TDL_OpenModel(stTDLHandle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, argv[1]);
    if (s32Ret != CVI_SUCCESS) {
        fprintf(stderr, "CVI_TDL_OpenModel failed for %s with %#x\n", argv[1], s32Ret);
        goto setup_tdl_fail;
    }

    if (argc == 3) {
        float fThreshold = atof(argv[2]);
        printf("Setting threshold to: %f\n", fThreshold);
        s32Ret = CVI_TDL_SetModelThreshold(stTDLHandle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, fThreshold);
        if (s32Ret != CVI_SUCCESS) {
            fprintf(stderr, "CVI_TDL_SetModelThreshold failed with %#x!\n", s32Ret);
            goto setup_tdl_fail;
        }
    }

    pthread_mutex_init(&g_queue_mutex, NULL);
    pthread_cond_init(&g_queue_cond_not_empty, NULL);
    pthread_cond_init(&g_queue_cond_not_full, NULL);

    ai_args.stTDLHandle = stTDLHandle;
    pthread_create(&stFrameThread, NULL, get_frame_thread, NULL);
    pthread_create(&stTDLThread, NULL, run_tdl_thread, &ai_args);

    printf("\nRunning detection... Press 's' to save a frame, Ctrl+C to exit.\n");
    while (bExit == false) {
        
        int c = getchar();
        if (c == 's' || c == 'S') {
            g_save_frame_flag = true;
        }
        usleep(100 * 100);
        
    }
    restore_terminal_mode();

    printf("\nShutdown signal received. Cleaning up...\n");
    pthread_mutex_lock(&g_queue_mutex);
    pthread_cond_broadcast(&g_queue_cond_not_empty);
    pthread_cond_broadcast(&g_queue_cond_not_full);
    pthread_mutex_unlock(&g_queue_mutex);

    pthread_join(stTDLThread, NULL);
    pthread_join(stFrameThread, NULL);
    printf("All threads have been joined.\n");

    pthread_mutex_destroy(&g_queue_mutex);
    pthread_cond_destroy(&g_queue_cond_not_empty);
    pthread_cond_destroy(&g_queue_cond_not_full);

setup_tdl_fail:
    if (stTDLHandle) {
        CVI_TDL_DestroyHandle(stTDLHandle);
        stTDLHandle = NULL;
    }
create_tdl_fail:
    sys_vi_deinit();

    printf("Program exited gracefully.\n");
    return s32Ret;
}