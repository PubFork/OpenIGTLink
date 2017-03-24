/*=========================================================================
 
 Program:   H264Encoder
 Language:  C++
 
 Copyright (c) Insight Software Consortium. All rights reserved.
 
 This software is distributed WITHOUT ANY WARRANTY; without even
 the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 PURPOSE.  See the above copyright notices for more information.
 
 =========================================================================*/

/*!
 * \copy
 *     Copyright (c)  2013, Cisco Systems
 *     All rights reserved.
 *
 *     Redistribution and use in source and binary forms, with or without
 *     modification, are permitted provided that the following conditions
 *     are met:
 *
 *        * Redistributions of source code must retain the above copyright
 *          notice, this list of conditions and the following disclaimer.
 *
 *        * Redistributions in binary form must reproduce the above copyright
 *          notice, this list of conditions and the following disclaimer in
 *          the documentation and/or other materials provided with the
 *          distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *     "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *     FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *     COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *     BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *     CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *     LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *     ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *     POSSIBILITY OF SUCH DAMAGE.
 *
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#if defined (ANDROID_NDK)
#include <android/log.h>
#endif
#ifdef ONLY_ENC_FRAMES_NUM
#undef ONLY_ENC_FRAMES_NUM
#endif//ONLY_ENC_FRAMES_NUM
#define ONLY_ENC_FRAMES_NUM INT_MAX // 2, INT_MAX // type the num you try to encode here, 2, 10, etc

#if defined (WINDOWS_PHONE)
float   g_fFPS           = 0.0;
double  g_dEncoderTime   = 0.0;
int     g_iEncodedFrame  = 0;
#endif

#if defined (ANDROID_NDK)
#define LOG_TAG "welsenc"
#define LOGI(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define printf(...) LOGI(__VA_ARGS__)
#define fprintf(a, ...) LOGI(__VA_ARGS__)
#endif
//#define STICK_STREAM_SIZE

#include "measure_time.h"


#include "typedefs.h"
#include "read_config.h"

#ifdef _MSC_VER
#include <io.h>     /* _setmode() */
#include <fcntl.h>  /* _O_BINARY */
#endif//_MSC_VER

#include "codec_def.h"
#include "codec_api.h"
#include "extern.h"
#include "macros.h"
#include "wels_const.h"

#include "mt_defs.h"
#include "WelsThreadLib.h"

#ifdef _WIN32
#ifdef WINAPI_FAMILY
#include <winapifamily.h>
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#define HAVE_PROCESS_AFFINITY
#endif
#else /* defined(WINAPI_FAMILY) */
#define HAVE_PROCESS_AFFINITY
#endif
#endif /* _WIN32 */

#if defined(__linux__) || defined(__unix__)
#define _FILE_OFFSET_BITS 64
#endif


#include "H264Encoder.h"


/* Ctrl-C handler */
static int     g_iCtrlC = 0;
static void    SigIntHandler (int a) {
  g_iCtrlC = 1;
}

H264Encoder::H264Encoder(char *configFile)
{
  this->pSVCEncoder = NULL;
  memset (&sFbi, 0, sizeof (SFrameBSInfo));
  
#ifdef _MSC_VER
  _setmode (_fileno (stdin), _O_BINARY);  /* thanks to Marcoss Morais <morais at dee.ufcg.edu.br> */
  _setmode (_fileno (stdout), _O_BINARY);
  
#endif
  
  /* Control-C handler */
  signal (SIGINT, SigIntHandler);
  
  this->configFile = std::string("");
  if (configFile!=NULL)
  {
    this->configFile = std::string(configFile);
  }
  this->InitializationDone = false;
  this->deviceName = "";
}

H264Encoder::~H264Encoder()
{
  WelsDestroySVCEncoder (this->pSVCEncoder);
  this->pSVCEncoder = NULL;
}

void H264Encoder::SetConfigurationFile(std::string configFile)
{
  this->configFile = std::string(configFile);
}

int H264Encoder::ParseLayerConfig (CReadConfig& cRdLayerCfg, const int iLayer, SEncParamExt& pSvcParam) {
  if (!cRdLayerCfg.ExistFile()) {
    fprintf (stderr, "Unabled to open layer #%d configuration file: %s.\n", iLayer, cRdLayerCfg.GetFileName().c_str());
    return 1;
  }
  
  SSpatialLayerConfig* pDLayer = &pSvcParam.sSpatialLayers[iLayer];
  int iLeftTargetBitrate = (pSvcParam.iRCMode != RC_OFF_MODE) ? pSvcParam.iTargetBitrate : 0;
  SLayerPEncCtx sLayerCtx;
  memset (&sLayerCtx, 0, sizeof (SLayerPEncCtx));
  
  string strTag[4];
  string str_ ("SlicesAssign");
  const int kiSize = (int)str_.size();
  
  while (!cRdLayerCfg.EndOfFile()) {
    long iLayerRd = cRdLayerCfg.ReadLine (&strTag[0]);
    if (iLayerRd > 0) {
      if (strTag[0].empty())
        continue;
      if (strTag[0].compare ("FrameWidth") == 0) {
        pDLayer->iVideoWidth = atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("FrameHeight") == 0) {
        pDLayer->iVideoHeight = atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("FrameRateOut") == 0) {
        pDLayer->fFrameRate = (float)atof (strTag[1].c_str());
      } else if (strTag[0].compare ("ProfileIdc") == 0) {
        pDLayer->uiProfileIdc = (EProfileIdc)atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("FRExt") == 0) {
        //        pDLayer->frext_mode = (bool)atoi(strTag[1].c_str());
      } else if (strTag[0].compare ("SpatialBitrate") == 0) {
        pDLayer->iSpatialBitrate = 1000 * atoi (strTag[1].c_str());
        if (pSvcParam.iRCMode != RC_OFF_MODE) {
          if (pDLayer->iSpatialBitrate <= 0) {
            fprintf (stderr, "Invalid spatial bitrate(%d) in dependency layer #%d.\n", pDLayer->iSpatialBitrate, iLayer);
            return -1;
          }
          if (pDLayer->iSpatialBitrate > iLeftTargetBitrate) {
            fprintf (stderr, "Invalid spatial(#%d) bitrate(%d) setting due to unavailable left(%d)!\n", iLayer,
                     pDLayer->iSpatialBitrate, iLeftTargetBitrate);
            return -1;
          }
          iLeftTargetBitrate -= pDLayer->iSpatialBitrate;
        }
      } else if (strTag[0].compare ("MaxSpatialBitrate") == 0) {
        pDLayer->iMaxSpatialBitrate = 1000 * atoi (strTag[1].c_str());
        if (pSvcParam.iRCMode != RC_OFF_MODE) {
          if (pDLayer->iMaxSpatialBitrate < 0) {
            fprintf (stderr, "Invalid max spatial bitrate(%d) in dependency layer #%d.\n", pDLayer->iMaxSpatialBitrate, iLayer);
            return -1;
          }
          if (pDLayer->iMaxSpatialBitrate > 0 && pDLayer->iMaxSpatialBitrate < pDLayer->iSpatialBitrate) {
            fprintf (stderr, "Invalid max spatial(#%d) bitrate(%d) setting::: < layerBitrate(%d)!\n", iLayer,
                     pDLayer->iMaxSpatialBitrate, pDLayer->iSpatialBitrate);
            return -1;
          }
        }
      } else if (strTag[0].compare ("InitialQP") == 0) {
        sLayerCtx.iDLayerQp = atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("SliceMode") == 0) {
        sLayerCtx.sSliceArgument.uiSliceMode = (SliceModeEnum)atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("SliceSize") == 0) { //SM_SIZELIMITED_SLICE
        sLayerCtx.sSliceArgument.uiSliceSizeConstraint = atoi (strTag[1].c_str());
        continue;
      } else if (strTag[0].compare ("SliceNum") == 0) {
        sLayerCtx.sSliceArgument.uiSliceNum = atoi (strTag[1].c_str());
      } else if (strTag[0].compare (0, kiSize, str_) == 0) {
        const char* kpString = strTag[0].c_str();
        int uiSliceIdx = atoi (&kpString[kiSize]);
        assert (uiSliceIdx < MAX_SLICES_NUM);
        sLayerCtx.sSliceArgument.uiSliceMbNum[uiSliceIdx] = atoi (strTag[1].c_str());
      }
    }
  }
  pDLayer->iDLayerQp             = sLayerCtx.iDLayerQp;
  pDLayer->sSliceArgument.uiSliceMode = sLayerCtx.sSliceArgument.uiSliceMode;
  
  memcpy (&pDLayer->sSliceArgument, &sLayerCtx.sSliceArgument, sizeof (SSliceArgument)); // confirmed_safe_unsafe_usage
  memcpy (&pDLayer->sSliceArgument.uiSliceMbNum[0], &sLayerCtx.sSliceArgument.uiSliceMbNum[0],
          sizeof (sLayerCtx.sSliceArgument.uiSliceMbNum)); // confirmed_safe_unsafe_usage
  
  return 0;
}

int H264Encoder::ParseConfig() {
  string strTag[4];
  int32_t iRet = 0;
  int8_t iLayerCount = 0;
  
  while (!cRdCfg.EndOfFile()) {
    long iRd = cRdCfg.ReadLine (&strTag[0]);
    if (iRd > 0) {
      if (strTag[0].empty())
        continue;
      
      if (strTag[0].compare ("UsageType") == 0) {
        sSvcParam.iUsageType = (EUsageType)atoi (strTag[1].c_str());
      }else if (strTag[0].compare ("SimulcastAVC") == 0) {
        sSvcParam.bSimulcastAVC = atoi (strTag[1].c_str()) ? true : false;
      } else if (strTag[0].compare ("TemporalLayerNum") == 0) {
        sSvcParam.iTemporalLayerNum = atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("IntraPeriod") == 0) {
        sSvcParam.uiIntraPeriod = atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("MaxNalSize") == 0) {
        sSvcParam.uiMaxNalSize = atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("SpsPpsIDStrategy") == 0) {
        int32_t iValue = atoi (strTag[1].c_str());
        switch (iValue) {
          case 0:
            sSvcParam.eSpsPpsIdStrategy  = CONSTANT_ID;
            break;
          case 0x01:
            sSvcParam.eSpsPpsIdStrategy  = INCREASING_ID;
            break;
          case 0x02:
            sSvcParam.eSpsPpsIdStrategy  = SPS_LISTING;
            break;
          case 0x03:
            sSvcParam.eSpsPpsIdStrategy  = SPS_LISTING_AND_PPS_INCREASING;
            break;
          case 0x06:
            sSvcParam.eSpsPpsIdStrategy  = SPS_PPS_LISTING;
            break;
          default:
            sSvcParam.eSpsPpsIdStrategy  = CONSTANT_ID;
            break;
        }
      } else if (strTag[0].compare ("EnableScalableSEI") == 0) {
        sSvcParam.bEnableSSEI = atoi (strTag[1].c_str()) ? true : false;
      } else if (strTag[0].compare ("EnableFrameCropping") == 0) {
        sSvcParam.bEnableFrameCroppingFlag = (atoi (strTag[1].c_str()) != 0);
      } else if (strTag[0].compare ("EntropyCodingModeFlag") == 0) {
        sSvcParam.iEntropyCodingModeFlag = (atoi (strTag[1].c_str()) != 0);
      } else if (strTag[0].compare ("LoopFilterDisableIDC") == 0) {
        sSvcParam.iLoopFilterDisableIdc = (int8_t)atoi (strTag[1].c_str());
        if (sSvcParam.iLoopFilterDisableIdc > 6 || sSvcParam.iLoopFilterDisableIdc < 0) {
          fprintf (stderr, "Invalid parameter in iLoopFilterDisableIdc: %d.\n", sSvcParam.iLoopFilterDisableIdc);
          iRet = 1;
          break;
        }
      } else if (strTag[0].compare ("LoopFilterAlphaC0Offset") == 0) {
        sSvcParam.iLoopFilterAlphaC0Offset = (int8_t)atoi (strTag[1].c_str());
        if (sSvcParam.iLoopFilterAlphaC0Offset < -6)
          sSvcParam.iLoopFilterAlphaC0Offset = -6;
        else if (sSvcParam.iLoopFilterAlphaC0Offset > 6)
          sSvcParam.iLoopFilterAlphaC0Offset = 6;
      } else if (strTag[0].compare ("LoopFilterBetaOffset") == 0) {
        sSvcParam.iLoopFilterBetaOffset = (int8_t)atoi (strTag[1].c_str());
        if (sSvcParam.iLoopFilterBetaOffset < -6)
          sSvcParam.iLoopFilterBetaOffset = -6;
        else if (sSvcParam.iLoopFilterBetaOffset > 6)
          sSvcParam.iLoopFilterBetaOffset = 6;
      } else if (strTag[0].compare ("MultipleThreadIdc") == 0) {
        // # 0: auto(dynamic imp. internal encoder); 1: multiple threads imp. disabled; > 1: count number of threads;
        sSvcParam.iMultipleThreadIdc = atoi (strTag[1].c_str());
        if (sSvcParam.iMultipleThreadIdc < 0)
          sSvcParam.iMultipleThreadIdc = 0;
        else if (sSvcParam.iMultipleThreadIdc > MAX_THREADS_NUM)
          sSvcParam.iMultipleThreadIdc = MAX_THREADS_NUM;
      } else if (strTag[0].compare ("UseLoadBalancing") == 0) {
        sSvcParam.bUseLoadBalancing = (atoi (strTag[1].c_str())) ? true : false;
      } else if (strTag[0].compare ("RCMode") == 0) {
        sSvcParam.iRCMode = (RC_MODES) atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("TargetBitrate") == 0) {
        sSvcParam.iTargetBitrate = 1000 * atoi (strTag[1].c_str());
        if ((sSvcParam.iRCMode != RC_OFF_MODE) && sSvcParam.iTargetBitrate <= 0) {
          fprintf (stderr, "Invalid target bitrate setting due to RC enabled. Check TargetBitrate field please!\n");
          return 1;
        }
      } else if (strTag[0].compare ("MaxOverallBitrate") == 0) {
        sSvcParam.iMaxBitrate = 1000 * atoi (strTag[1].c_str());
        if ((sSvcParam.iRCMode != RC_OFF_MODE) && sSvcParam.iMaxBitrate < 0) {
          fprintf (stderr, "Invalid max overall bitrate setting due to RC enabled. Check MaxOverallBitrate field please!\n");
          return 1;
        }
      } else if (strTag[0].compare ("MaxQp") == 0) {
        sSvcParam.iMaxQp = atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("MinQp") == 0) {
        sSvcParam.iMinQp = atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("EnableDenoise") == 0) {
        sSvcParam.bEnableDenoise = atoi (strTag[1].c_str()) ? true : false;
      } else if (strTag[0].compare ("EnableSceneChangeDetection") == 0) {
        sSvcParam.bEnableSceneChangeDetect = atoi (strTag[1].c_str()) ? true : false;
      } else if (strTag[0].compare ("EnableBackgroundDetection") == 0) {
        sSvcParam.bEnableBackgroundDetection = atoi (strTag[1].c_str()) ? true : false;
      } else if (strTag[0].compare ("EnableAdaptiveQuantization") == 0) {
        sSvcParam.bEnableAdaptiveQuant = atoi (strTag[1].c_str()) ? true : false;
      } else if (strTag[0].compare ("EnableFrameSkip") == 0) {
        sSvcParam.bEnableFrameSkip = atoi (strTag[1].c_str()) ? true : false;
      } else if (strTag[0].compare ("EnableLongTermReference") == 0) {
        sSvcParam.bEnableLongTermReference = atoi (strTag[1].c_str()) ? true : false;
      } else if (strTag[0].compare ("LongTermReferenceNumber") == 0) {
        sSvcParam.iLTRRefNum = atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("LtrMarkPeriod") == 0) {
        sSvcParam.iLtrMarkPeriod = (uint32_t)atoi (strTag[1].c_str());
      } else if (strTag[0].compare ("LosslessLink") == 0) {
        sSvcParam.bIsLosslessLink = atoi (strTag[1].c_str()) ? true : false;
      } else if (strTag[0].compare ("NumLayers") == 0) {
        sSvcParam.iSpatialLayerNum = (int8_t)atoi (strTag[1].c_str());
        if (sSvcParam.iSpatialLayerNum > MAX_DEPENDENCY_LAYER || sSvcParam.iSpatialLayerNum <= 0) {
          fprintf (stderr, "Invalid parameter in iSpatialLayerNum: %d.\n", sSvcParam.iSpatialLayerNum);
          iRet = 1;
          break;
        }
      } else if (strTag[0].compare ("LayerCfg") == 0) {
        if (strTag[1].length() > 0)
          layerConfigFiles[iLayerCount] = strTag[1];
        //          pSvcParam.sDependencyLayers[iLayerCount].uiDependencyId = iLayerCount;
        ++ iLayerCount;
      } else if (strTag[0].compare ("PrefixNALAddingCtrl") == 0) {
        int ctrl_flag = atoi (strTag[1].c_str());
        if (ctrl_flag > 1)
          ctrl_flag = 1;
        else if (ctrl_flag < 0)
          ctrl_flag = 0;
        sSvcParam.bPrefixNalAddingCtrl = ctrl_flag ? true : false;
      }
    }
  }
  
  const int8_t kiActualLayerNum = WELS_MIN (sSvcParam.iSpatialLayerNum, iLayerCount);
  if (sSvcParam.iSpatialLayerNum >
      kiActualLayerNum) { // fixed number of dependency layer due to parameter error in settings
    sSvcParam.iSpatialLayerNum = kiActualLayerNum;
  }
  
  assert (kiActualLayerNum <= MAX_DEPENDENCY_LAYER);
  
  for (int8_t iLayer = 0; iLayer < kiActualLayerNum; ++ iLayer) {
    CReadConfig cRdLayerCfg (layerConfigFiles[iLayer]);
    if (-1 == ParseLayerConfig (cRdLayerCfg, iLayer, sSvcParam)) {
      iRet = 1;
      break;
    }
  }
  
  return iRet;
}

bool H264Encoder::InitializeEncoder()
{
  //------------------------------------------------------------
  int iRet = 0;
  iRet = WelsCreateSVCEncoder(&(this->pSVCEncoder));
  
  // Preparing encoding process
  
  // Inactive with sink with output file handler
  this->pSVCEncoder->GetDefaultParams (&sSvcParam);
  
  //FillSpecificParameters (sSvcParam);
  // if configure file exit, reading configure file firstly
  if (this->configFile=="")
  {
    fprintf (stderr, "No configuration file specified. \n");
    iRet = 1;
    goto INSIDE_MEM_FREE;
  }
  
  cRdCfg.Openf (this->configFile.c_str());// to do get the first augments from this->augments.
  if (cRdCfg.ExistFile())
  {
    iRet = ParseConfig();
    if (iRet) {
      fprintf (stderr, "parse svc parameter config file failed.\n");
      iRet = 1;
      goto INSIDE_MEM_FREE;
    }
  }
  else
  {
    fprintf (stderr, "Specified file: %s not exist, maybe invalid path or parameter settting.\n",
             cRdCfg.GetFileName().c_str());
    iRet = 1;
    goto INSIDE_MEM_FREE;
  }
  static int     g_LevelSetting = WELS_LOG_ERROR;
  this->pSVCEncoder->SetOption (ENCODER_OPTION_TRACE_LEVEL, &g_LevelSetting);
  //finish reading the configuration
  
  //  sSvcParam.bSimulcastAVC = true;
  if (cmResultSuccess != this->pSVCEncoder->InitializeExt (&sSvcParam)) { // SVC encoder initialization
    fprintf (stderr, "SVC encoder Initialize failed\n");
    iRet = 1;
    goto INSIDE_MEM_FREE;
  }

  this->InitializationDone = true;
  return true;
  
INSIDE_MEM_FREE:
  this->InitializationDone = false;
  return false;
}

int H264Encoder::EncodeSingleFrameIntoVideoMSG(SSourcePicture* pSrcPic, igtl::VideoMessage* videoMessage, bool isGrayImage)
{
  int encodeRet = -1;
  if (this->InitializationDone == true)
  {
    int iSourceWidth = pSrcPic->iPicWidth;
    int iSourceHeight = pSrcPic->iPicHeight;
    pSrcPic->iStride[0] = iSourceWidth;
    pSrcPic->iStride[1] = pSrcPic->iStride[2] = pSrcPic->iStride[0] >> 1;
    int kiPicResSize = iSourceWidth * iSourceHeight * 3 >> 1;
    if(this->useCompress)
    {
      static igtl_uint32 messageID = -1;
      int frameSize = 0;
      int iLayer = 0;
      encodeRet = pSVCEncoder->EncodeFrame(pSrcPic, &sFbi);
      videoMessage->SetBitStreamSize(sFbi.iFrameSizeInBytes);
      videoMessage->AllocateBuffer();
      videoMessage->SetScalarType(videoMessage->TYPE_UINT8);
      videoMessage->SetEndian(igtl_is_little_endian()==true?2:1); //little endian is 2 big endian is 1
      videoMessage->SetWidth(pSrcPic->iPicWidth);
      videoMessage->SetHeight(pSrcPic->iPicHeight);
      encodedFrameType = sFbi.eFrameType;
      if (isGrayImage)
      {
        encodedFrameType = sFbi.eFrameType<<8;
      }
      videoMessage->SetFrameType(encodedFrameType);
      messageID ++;
      videoMessage->SetMessageID(messageID);
      while (iLayer < sFbi.iLayerNum) {
        SLayerBSInfo* pLayerBsInfo = &sFbi.sLayerInfo[iLayer];
        if (pLayerBsInfo != NULL) {
          int iLayerSize = 0;
          int iNalIdx = pLayerBsInfo->iNalCount - 1;
          do {
            iLayerSize += pLayerBsInfo->pNalLengthInByte[iNalIdx];
            -- iNalIdx;
          } while (iNalIdx >= 0);
          frameSize += iLayerSize;
          for (int i = 0; i < iLayerSize ; i++)
          {
            videoMessage->GetPackFragmentPointer(2)[frameSize-iLayerSize+i] = pLayerBsInfo->pBsBuf[i];
          }
        }
        ++ iLayer;
      }
    }
    else
    {
      static igtl_uint32 messageID = -1;
      videoMessage->SetHeaderVersion(IGTL_HEADER_VERSION_2);
      videoMessage->SetDeviceName(this->deviceName.c_str());
      videoMessage->SetBitStreamSize(kiPicResSize);
      videoMessage->AllocateBuffer();
      videoMessage->SetScalarType(videoMessage->TYPE_UINT8);
      videoMessage->SetEndian(igtl_is_little_endian()==true?IGTL_VIDEO_ENDIAN_LITTLE:IGTL_VIDEO_ENDIAN_BIG); //little endian is 2 big endian is 1
      videoMessage->SetWidth(pSrcPic->iPicWidth);
      videoMessage->SetHeight(pSrcPic->iPicHeight);
      encodedFrameType = videoFrameTypeIDR;
      if (isGrayImage)
      {
        encodedFrameType = videoFrameTypeIDR<<8;
      }
      videoMessage->SetFrameType(encodedFrameType);
      messageID ++;
      videoMessage->SetMessageID(messageID);
      memcpy(videoMessage->m_Frame, pSrcPic->pData[0], kiPicResSize);
    }
    videoMessage->Pack();
    
  }
  return encodeRet;
}