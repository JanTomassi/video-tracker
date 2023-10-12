#include <SDL2/SDL.h>
#include <SDL2/SDL_error.h>
#include <SDL2/SDL_pixels.h>
#include <SDL2/SDL_rect.h>
#include <SDL2/SDL_render.h>
#include <SDL2/SDL_stdinc.h>
#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_video.h>
#include <asm-generic/errno-base.h>
#include <bits/pthreadtypes.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <unistd.h>

#define ERROR
#define LOG
#define DIE(X) die(__builtin_FUNCTION(), __builtin_LINE(), X)
#define DIE_AV(X) die_av(__builtin_FUNCTION(), __builtin_LINE(), X)
#define DIE_SDL() die_sdl(__builtin_FUNCTION(), __builtin_LINE())

__attribute__((noreturn)) void die_av(const char *where, const int line,
                                      int code) {
#ifdef ERROR
  fprintf(stderr, "\033[31;5mDie_av(%s:%d): %s\033[0m\n", where, line,
          av_err2str(code));
#endif
  exit(1);
}

__attribute__((noreturn)) void die_sdl(const char *where, const int line) {
#ifdef ERROR
  fprintf(stderr, "\033[31;5mDie_sdl(%s:%d): %s\033[0m\n", where, line,
          SDL_GetError());
#endif
  exit(1);
}

__attribute__((noreturn)) void die(const char *where, const int line,
                                   const char *reason) {
#ifdef ERROR
  fprintf(stderr, "\033[31;5mDie(%s:%d): %s\033[0m\n", where, line, reason);
#endif
  exit(1);
}

typedef struct {
  AVFormatContext *formatCtx;
  AVCodecContext *codecCtx;
  const AVCodec *videoCodec;
  AVPacket *pkt;
  AVFrame *firstFrame;
  AVFrame *secondFrame;
  int videoStream;
} avCtx;

typedef struct {
  SDL_Window *win;
  SDL_Renderer *ren;
  SDL_Texture *fstTex;
  SDL_Texture *sndTex;
  SDL_Texture *fstkeyTex;
  SDL_Texture *sndkeyTex;
} sdlCtx;

typedef struct {
  struct SwsContext *swsctx_gray;
  pthread_mutex_t *gray_mutex;
  struct SwsContext *swsctx_gray_rgb;
  pthread_mutex_t *gray_rgb_mutex;
  AVFrame *srcFrame;
} FAST_thread_args;

typedef struct {
  int x;
  int y;
} point;

typedef struct FAST_desc {
  point point;
  float score;
} FAST_descs;

__attribute_pure__ avCtx *allocate_av();
__attribute_pure__ avCtx *init_av(char *filepath);
void free_av(avCtx *avctx);

__attribute_pure__ sdlCtx *allocate_sdl();
__attribute_pure__ sdlCtx *init_sdl(sdlCtx *sdlctx, int w, int h, Uint32 flags);
void free_sdl(sdlCtx *ptr);

__attribute_pure__ avCtx *allocate_av() {
  avCtx *res = calloc(1, sizeof(avCtx));
  res->videoStream = -1;
  return res;
}

__attribute_pure__ avCtx *init_av(char *filepath) {
  avCtx *avctx = allocate_av();

  int error;
  if ((error = avformat_open_input(&avctx->formatCtx, filepath, NULL, NULL)) !=
      0) {
    free_av(avctx);
    DIE_AV(error);
  }

  if ((error = avformat_find_stream_info(avctx->formatCtx, NULL)) != 0) {
    free_av(avctx);
    DIE_AV(error);
  }

  av_dump_format(avctx->formatCtx, 0, filepath, 0);

  /* Find the first video stream or die */
  for (int i = 0; i < avctx->formatCtx->nb_streams; i++)
    if (avctx->formatCtx->streams[i]->codecpar->codec_type ==
        AVMEDIA_TYPE_VIDEO) {
      avctx->videoStream = i;
      break;
    }
  if (avctx->videoStream == -1) {
    free_av(avctx);
    DIE("Couldn't find a video stream");
  }

  if ((avctx->videoCodec =
           avcodec_find_decoder(avctx->formatCtx->streams[avctx->videoStream]
                                    ->codecpar->codec_id)) == 0) {
    free_av(avctx);
    DIE("Couldn't find a valid decoder");
  }

  if ((avctx->codecCtx = avcodec_alloc_context3(avctx->videoCodec)) == 0) {
    free_av(avctx);
    DIE("Couldn't initialize the codec context");
  }

  if ((error = avcodec_parameters_to_context(
           avctx->codecCtx,
           avctx->formatCtx->streams[avctx->videoStream]->codecpar)) < 0) {
    free_av(avctx);
    DIE_AV(error);
  }

  if ((error = avcodec_open2(avctx->codecCtx, avctx->videoCodec, NULL)) != 0) {
    free_av(avctx);
    DIE_AV(error);
  }

#ifdef LOG
  printf("Log: the codec selected for the first video stream is: %s\n",
         avctx->videoCodec->long_name);
#endif

  if ((avctx->pkt = av_packet_alloc()) == 0) {
    free_av(avctx);
    DIE("Couldn't allocate package");
  }

  if ((avctx->firstFrame = av_frame_alloc()) == 0) {
    free_av(avctx);
    DIE("Couldn't allocate frame");
  }
  if ((avctx->secondFrame = av_frame_alloc()) == 0) {
    free_av(avctx);
    DIE("Couldn't allocate frame");
  }

  return avctx;
}

void free_av(avCtx *avctx) {
  if (avctx->codecCtx) avcodec_free_context(&avctx->codecCtx);
  if (avctx->formatCtx) avformat_close_input(&avctx->formatCtx);
  if (avctx->videoCodec) avctx->videoCodec = NULL;
  if (avctx->firstFrame) av_frame_free(&avctx->firstFrame);
  if (avctx->secondFrame) av_frame_free(&avctx->secondFrame);
  if (avctx->pkt) av_packet_free(&avctx->pkt);
  free(avctx);
}

__attribute_pure__ sdlCtx *allocate_sdl() {
  sdlCtx *res = calloc(1, sizeof(sdlCtx));
  return res;
}

__attribute_pure__ sdlCtx *init_sdl(sdlCtx *sdlctx, int w, int h,
                                    Uint32 flags) {
  sdlctx->win = SDL_CreateWindow("Jan - Tracker", SDL_WINDOWPOS_UNDEFINED,
                                 SDL_WINDOWPOS_UNDEFINED, w, h, flags);
  if (sdlctx->win == 0) DIE(SDL_GetError());

  sdlctx->ren = SDL_CreateRenderer(sdlctx->win, -1, SDL_RENDERER_ACCELERATED);
  if (sdlctx->ren == 0) DIE(SDL_GetError());

  sdlctx->fstTex = SDL_CreateTexture(sdlctx->ren, SDL_PIXELFORMAT_RGB24,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
  if (sdlctx->fstTex == 0) DIE(SDL_GetError());

  sdlctx->fstkeyTex = SDL_CreateTexture(sdlctx->ren, SDL_PIXELFORMAT_RGB24,
                                        SDL_TEXTUREACCESS_STREAMING, w, h);
  if (sdlctx->fstkeyTex == 0) DIE(SDL_GetError());

  sdlctx->sndTex = SDL_CreateTexture(sdlctx->ren, SDL_PIXELFORMAT_RGB24,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
  if (sdlctx->sndTex == 0) DIE(SDL_GetError());

  sdlctx->sndkeyTex = SDL_CreateTexture(sdlctx->ren, SDL_PIXELFORMAT_RGB24,
                                        SDL_TEXTUREACCESS_STREAMING, w, h);
  if (sdlctx->sndkeyTex == 0) DIE(SDL_GetError());
  return sdlctx;
}

void free_sdl(sdlCtx *ptr) {
  SDL_DestroyTexture(ptr->fstTex);
  SDL_DestroyTexture(ptr->sndTex);
  SDL_DestroyTexture(ptr->fstkeyTex);
  SDL_DestroyTexture(ptr->sndkeyTex);
  SDL_DestroyRenderer(ptr->ren);
  SDL_DestroyWindowSurface(ptr->win);
  free(ptr);
}

__attribute_warn_unused_result__ char get_one_valid_pkt(avCtx *avctx) {
  int error;
  do {
    av_packet_unref(avctx->pkt);
    error = av_read_frame(avctx->formatCtx, avctx->pkt);
    if (error == AVERROR_EOF)
      return 1;
    else if (error < 0)
      DIE_AV(error);
  } while (avctx->pkt->stream_index != avctx->videoStream);
  return 0;
}

char get_one_frame(avCtx *avctx, AVFrame *frame) {
  int error;
  error = avcodec_send_packet(avctx->codecCtx, avctx->pkt);
  if (error < 0) DIE_AV(error);
  error = avcodec_receive_frame(avctx->codecCtx, frame);
  if (error == AVERROR(EAGAIN)) return 1;
  if (error == AVERROR_EOF) return 2;
  if (error < 0) DIE_AV(error);
  return 0;
}

static uint8_t *get_element(uint8_t *data, int width, int height, int pw,
                            int ph) {
  if (width <= 0) DIE("width to small");
  if (height <= 0) DIE("height to small");

  // width clamping
  if (pw >= width)
    pw = width - 1;
  else if (pw < 0)
    pw = 0;

  // height clamping
  if (ph >= height)
    ph = height - 1;
  else if (ph < 0)
    ph = 0;

  return (data + pw) + (ph * width);
}

void FAST_point_test(uint8_t *data, int w, int h, uint8_t *res) {
  uint8_t p;
  uint8_t t = 16;
  uint8_t in_t = 0;
  uint32_t score = 0;
  for (int pw = 0; pw < w; pw++)
    for (int ph = 0; ph < h; ph++) {
      p = *get_element(data, w, h, pw, ph);
      uint8_t p1 = *get_element(data, w, h, pw, ph - 3);
      uint8_t p5 = *get_element(data, w, h, pw + 3, ph);
      uint8_t p9 = *get_element(data, w, h, pw, ph + 3);
      uint8_t p13 = *get_element(data, w, h, pw - 3, ph);
      if (!((p1 - t > p || p1 + t < p) && (p5 - t > p || p5 + t < p) &&
            (p9 - t > p || p9 + t < p) && (p13 - t > p || p13 + t < p)))
        continue;
      in_t = 4;
      uint8_t p2 = *get_element(data, w, h, pw + 1, ph - 3);
      if (p2 - t > p || p2 + t < p) in_t++;
      uint8_t p3 = *get_element(data, w, h, pw + 2, ph - 2);
      if (p3 - t > p || p3 + t < p) in_t++;
      uint8_t p4 = *get_element(data, w, h, pw + 3, ph - 1);
      if (p4 - t > p || p4 + t < p) in_t++;
      uint8_t p6 = *get_element(data, w, h, pw + 3, ph + 1);
      if (p6 - t > p || p6 + t < p) in_t++;
      uint8_t p7 = *get_element(data, w, h, pw + 2, ph + 2);
      if (p7 - t > p || p7 + t < p) in_t++;
      uint8_t p8 = *get_element(data, w, h, pw + 1, ph + 3);
      if (p8 - t > p || p8 + t < p) in_t++;
      uint8_t p10 = *get_element(data, w, h, pw - 1, ph + 3);
      if (p10 - t > p || p10 + t < p) in_t++;
      uint8_t p11 = *get_element(data, w, h, pw - 2, ph + 2);
      if (p11 - t > p || p11 + t < p) in_t++;
      uint8_t p12 = *get_element(data, w, h, pw - 3, ph + 1);
      if (p12 - t > p || p12 + t < p) in_t++;
      uint8_t p14 = *get_element(data, w, h, pw - 3, ph - 1);
      if (p14 - t > p || p14 + t < p) in_t++;
      uint8_t p15 = *get_element(data, w, h, pw - 2, ph - 2);
      if (p15 - t > p || p15 + t < p) in_t++;
      uint8_t p16 = *get_element(data, w, h, pw - 1, ph - 3);
      if (p16 - t > p || p16 + t < p) in_t++;

      if (in_t > 12) {
        uint8_t *point = get_element(res, w, h, pw, ph);
        *point = ((p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9 + p10 + p11 +
                   p12 + p13 + p14 + p15 + p16) /
                  16.0);
      }
    }
}

void *FAST_render_thread(void *args) {
  FAST_thread_args *largs = (FAST_thread_args *)args;

  AVFrame *grayFrame = av_frame_alloc();
  grayFrame->format = AV_PIX_FMT_GRAY8;
  grayFrame->width = largs->srcFrame->width;
  grayFrame->height = largs->srcFrame->height;
  av_frame_get_buffer(grayFrame, 0);

  AVFrame *fastResFrame = av_frame_alloc();
  fastResFrame->format = AV_PIX_FMT_GRAY8;
  fastResFrame->width = largs->srcFrame->width;
  fastResFrame->height = largs->srcFrame->height;
  av_frame_get_buffer(fastResFrame, 0);
  memset(fastResFrame->data[0], 128,
         fastResFrame->linesize[0] * fastResFrame->height);

  AVFrame *rgbFrame = av_frame_alloc();
  rgbFrame->format = AV_PIX_FMT_RGB24;
  rgbFrame->width = largs->srcFrame->width;
  rgbFrame->height = largs->srcFrame->height;
  av_frame_get_buffer(rgbFrame, 0);

  pthread_mutex_lock(largs->gray_mutex);
  sws_scale(largs->swsctx_gray, largs->srcFrame->data,
            largs->srcFrame->linesize, 0, largs->srcFrame->height,
            grayFrame->data, grayFrame->linesize);
  pthread_mutex_unlock(largs->gray_mutex);

  FAST_point_test(grayFrame->data[0], grayFrame->width, grayFrame->height,
                  fastResFrame->data[0]);

  pthread_mutex_lock(largs->gray_rgb_mutex);
  sws_scale(largs->swsctx_gray_rgb, fastResFrame->data, fastResFrame->linesize,
            0, fastResFrame->height, rgbFrame->data, rgbFrame->linesize);
  pthread_mutex_unlock(largs->gray_rgb_mutex);

  av_frame_free(&fastResFrame);
  av_frame_free(&grayFrame);
  return rgbFrame;
}

int main(int argc, char **argv) {
  char opt = -1;
  char *filepath = malloc(0);
  while ((opt = getopt(argc, argv, "f:")) != -1) switch (opt) {
      case 'f':
        filepath = realloc(filepath, (strlen(optarg)) + 1);
        filepath = strncpy(filepath, optarg, strlen(optarg) + 1);
        break;
      default:
        exit(0);
    };

#ifdef LOG
  av_log_set_level(AV_LOG_INFO);
  printf("Log: You have select the file %s\n", filepath);
#endif

  avCtx *avctx = init_av(filepath);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) DIE_SDL();

  struct SwsContext *swsContext_gray = sws_getContext(
      avctx->codecCtx->width, avctx->codecCtx->height, avctx->codecCtx->pix_fmt,
      avctx->codecCtx->width, avctx->codecCtx->height, AV_PIX_FMT_GRAY8,
      SWS_BILINEAR, NULL, NULL, NULL);
  pthread_mutex_t gray_mutex;
  pthread_mutex_init(&gray_mutex, NULL);

  struct SwsContext *swsContext_rgb = sws_getContext(
      avctx->codecCtx->width, avctx->codecCtx->height, avctx->codecCtx->pix_fmt,
      avctx->codecCtx->width, avctx->codecCtx->height, AV_PIX_FMT_RGB24,
      SWS_BILINEAR, NULL, NULL, NULL);

  struct SwsContext *swsContext_gray_rgb = sws_getContext(
      avctx->codecCtx->width, avctx->codecCtx->height, AV_PIX_FMT_GRAY8,
      avctx->codecCtx->width, avctx->codecCtx->height, AV_PIX_FMT_RGB24,
      SWS_BILINEAR, NULL, NULL, NULL);
  pthread_mutex_t gray_rgb_mutex;
  pthread_mutex_init(&gray_rgb_mutex, NULL);

  AVFrame *rgbFrame = av_frame_alloc();
  if (!rgbFrame) DIE("Coudn't allocate frame");

#if 0
  AVFrame *grayFrame = av_frame_alloc();
  if (!rgbFrame) DIE("Coudn't allocate frame");
#endif

  AVFrame *fastResFrame = av_frame_alloc();
  if (!fastResFrame) DIE("Coudn't allocate frame");

  sdlCtx *sdlctx = allocate_sdl();
  sdlctx = init_sdl(sdlctx, avctx->codecCtx->width, avctx->codecCtx->height, 0);

  char eof;
  char eagain;
  do {
    eof = get_one_valid_pkt(avctx);
    eagain = get_one_frame(avctx, avctx->secondFrame);
    av_packet_unref(avctx->pkt);
  } while (!eof && eagain);

  SDL_Event event;
  char running = 1;
  while (running) {
    SDL_PollEvent(&event);
    if (event.type == SDL_QUIT) {
      running = 0;
      break;
    }

    av_frame_unref(avctx->firstFrame);
    av_frame_move_ref(avctx->firstFrame, avctx->secondFrame);

    char eof = get_one_valid_pkt(avctx);
    if (eof) running = 0;

    char status = get_one_frame(avctx, avctx->secondFrame);
    if (status == 1) continue;
    if (status == 2) break;

    rgbFrame->format = AV_PIX_FMT_RGB24;
    rgbFrame->width = avctx->codecCtx->width;
    rgbFrame->height = avctx->codecCtx->height;
#if 0
    // Set the properties of the RGB24 frame
    grayFrame->format = AV_PIX_FMT_GRAY8;
    grayFrame->width = avctx->codecCtx->width;
    grayFrame->height = avctx->codecCtx->height;
#endif

    // Set the properties of the RGB24 frame
    fastResFrame->format = AV_PIX_FMT_GRAY8;
    fastResFrame->width = avctx->codecCtx->width;
    fastResFrame->height = avctx->codecCtx->height;

    av_frame_get_buffer(rgbFrame, 0);
    av_frame_get_buffer(fastResFrame, 0);
    memset(fastResFrame->data[0], 0,
           fastResFrame->linesize[0] * fastResFrame->height);

    int sdl_error = 0;

    sdl_error = SDL_RenderClear(sdlctx->ren);
    if (sdl_error < 0) DIE_SDL();

    // First rect
    pthread_t FAST_thread_fst;
    pthread_t FAST_thread_snd;
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    FAST_thread_args FAST_args_fst = {.gray_mutex = &gray_mutex,
                                      .gray_rgb_mutex = &gray_rgb_mutex,
                                      .swsctx_gray = swsContext_gray,
                                      .swsctx_gray_rgb = swsContext_gray_rgb,
                                      .srcFrame = avctx->firstFrame->data};
    FAST_thread_args FAST_args_snd = {.gray_mutex = &gray_mutex,
                                      .gray_rgb_mutex = &gray_rgb_mutex,
                                      .swsctx_gray = swsContext_gray,
                                      .swsctx_gray_rgb = swsContext_gray_rgb,
                                      .srcFrame = avctx->secondFrame->data};

    AVFrame *res;

    pthread_create(&FAST_thread_fst, &attr, FAST_render_thread, &FAST_args_fst);
    pthread_create(&FAST_thread_snd, &attr, FAST_render_thread, &FAST_args_snd);
    pthread_attr_destroy(&attr);

    sws_scale(swsContext_rgb, avctx->firstFrame->data,
              avctx->firstFrame->linesize, 0, avctx->firstFrame->height,
              rgbFrame->data, rgbFrame->linesize);

    SDL_Rect firstRect = {.x = 0,
                          .y = 0,
                          .w = avctx->codecCtx->width / 2,
                          .h = avctx->codecCtx->height / 2};

    sdl_error = SDL_UpdateTexture(sdlctx->fstTex, NULL, rgbFrame->data[0],
                                  rgbFrame->linesize[0]);
    if (sdl_error < 0) DIE_SDL();

    sdl_error = SDL_RenderCopy(sdlctx->ren, sdlctx->fstTex, NULL, &firstRect);
    if (sdl_error < 0) DIE_SDL();

    sws_scale(swsContext_rgb, avctx->secondFrame->data,
              avctx->firstFrame->linesize, 0, avctx->firstFrame->height,
              rgbFrame->data, rgbFrame->linesize);

    // Second rect
    SDL_Rect secondRect = {.x = avctx->codecCtx->width / 2,
                           .y = 0,
                           .w = avctx->codecCtx->width / 2,
                           .h = avctx->codecCtx->height / 2};

    sdl_error = SDL_UpdateTexture(sdlctx->fstTex, NULL, rgbFrame->data[0],
                                  rgbFrame->linesize[0]);
    if (sdl_error < 0) DIE_SDL();

    sdl_error = SDL_RenderCopy(sdlctx->ren, sdlctx->fstTex, NULL, &secondRect);
    if (sdl_error < 0) DIE_SDL();

    SDL_Rect firstkeyRect = {.x = 0,
                             .y = avctx->codecCtx->height / 2,
                             .w = avctx->codecCtx->width / 2,
                             .h = avctx->codecCtx->height / 2};

    pthread_join(FAST_thread_fst, (void **)&res);
    sdl_error = SDL_UpdateTexture(sdlctx->fstkeyTex, NULL, res->data[0],
                                  res->linesize[0]);
    if (sdl_error < 0) DIE_SDL();
    sdl_error =
        SDL_RenderCopy(sdlctx->ren, sdlctx->fstkeyTex, NULL, &firstkeyRect);
    if (sdl_error < 0) DIE_SDL();
    av_frame_free(&res);

    SDL_Rect secondkeyRect = {.x = avctx->codecCtx->width / 2,
                              .y = avctx->codecCtx->height / 2,
                              .w = avctx->codecCtx->width / 2,
                              .h = avctx->codecCtx->height / 2};

    pthread_join(FAST_thread_snd, (void **)&res);
    sdl_error = SDL_UpdateTexture(sdlctx->sndkeyTex, NULL, res->data[0],
                                  res->linesize[0]);
    if (sdl_error < 0) DIE_SDL();
    sdl_error =
        SDL_RenderCopy(sdlctx->ren, sdlctx->sndkeyTex, NULL, &secondkeyRect);
    if (sdl_error < 0) DIE_SDL();
    av_frame_free(&res);

    SDL_RenderPresent(sdlctx->ren);

    av_frame_unref(avctx->firstFrame);
    av_frame_unref(rgbFrame);
    av_frame_unref(fastResFrame);
    av_packet_unref(avctx->pkt);
    // SDL_Delay(16);
  }

  av_frame_free(&rgbFrame);
  av_frame_free(&fastResFrame);

  sws_freeContext(swsContext_rgb);
  pthread_mutex_destroy(&gray_mutex);
  sws_freeContext(swsContext_gray);
  pthread_mutex_destroy(&gray_rgb_mutex);
  sws_freeContext(swsContext_gray_rgb);

  free_sdl(sdlctx);
  SDL_Quit();

  free_av(avctx);
  free(filepath);

  return 0;
}
