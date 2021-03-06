#include "zeitengine.h"

ZeitEngine::ZeitEngine(GLVideoWidget* video_widget, QObject *parent) :
    QObject(parent)
{
    static bool avglobals_initialized = false;

    operation_mode = ZEIT_MODE_ZD;

    if(!avglobals_initialized) {
        av_register_all();
        avcodec_register_all();
        avfilter_register_all();
        avglobals_initialized = true;
    }

    QScreen* screen = QApplication::primaryScreen();

    display_refresh_rate = screen->refreshRate();
    display_safe_max_width = std::min((int)(screen->availableSize().width() * 0.66), screen->availableSize().width() - 200);
    display_safe_max_height = std::min((int)(screen->availableSize().height() * 0.66), screen->availableSize().height() - 200);
    display = video_widget;
    display_initialized = false;
    rotation_initialized = false;

    configured_framerate = ZEIT_RATE_24p;

    decoder_format = av_find_input_format("image2");
    decoder_format_context = NULL;
    decoder_codec = NULL;
    decoder_codec_context = NULL;
    decoder_frame = NULL;
    decoder_packet = NULL;

    scaler_context = NULL;
    scaler_frame = NULL;
    scaler_initialized = false;

    rescaler_context = NULL;
    rescaler_frame = NULL;
    rescaler_initialized = false;

    configured_filter = ZEIT_FILTER_NONE;
    filter_initialized = false;

    control_mutex.lock();
    stop_flag = false;
    loop_flag = true;
    filter_flag = ZEIT_FILTER_NONE;
    flip_x_flag = true;
    flip_y_flag = true;
    rotate_90d_cw_flag = false;
    control_mutex.unlock();

    preview_flag = false;

    exporter_initialized = false;
}

ZeitEngine::~ZeitEngine()
{
    FreeDecoder();
    FreeScaler();
    FreeFilter();
    FreeFilterData();
    FreeRescaler();
}

void ZeitEngine::InitDecoder()
{
    int ret;

    try
    {
        QString first_image = (*source_sequence.constBegin()).absoluteFilePath();

        AVDictionary *options = NULL;

        if(operation_mode == ZEIT_MODE_ZD) {
            av_dict_set(&options, "video_size", "1944x1944", 0);
            av_dict_set(&options, "pixel_format", "bayer_grbg16le", 0);
        }

        // Open input file
        if ((ret = avformat_open_input(&decoder_format_context, first_image.toUtf8().data(), decoder_format, &options)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to open input file '%s'\n", first_image.toUtf8().data());
            throw(ret);
        }

        // TODO: Check if freeing is a problem if not setting (probably not, quick evaluation)
        av_dict_free(&options);

        if(operation_mode == ZEIT_MODE_ZD) {
            decoder_format_context->streams[0]->codec->codec_id = AV_CODEC_ID_RAWVIDEO;
        }

        // Find decoder codec
        decoder_codec_context = decoder_format_context->streams[0]->codec;
        if( !(decoder_codec = avcodec_find_decoder(decoder_codec_context->codec_id)) ) {
            av_log(NULL, AV_LOG_ERROR, "Failed to find input codec\n");
            ret = AVERROR(EINVAL);
            throw(ret);
        }

        // Open decoder codec
        if( (ret = avcodec_open2(decoder_codec_context, decoder_codec, NULL)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to open input codec\n");
            throw(ret);
        }

        // Allocate decoder frame
        if( !(decoder_frame = av_frame_alloc()) ) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate decoder frame\n");
            ret = AVERROR(ENOMEM);
            throw(ret);
        }
    }
    catch(int code) {
        av_log(NULL, AV_LOG_ERROR, "Return code was %d", code);
    }
}

void ZeitEngine::FreeDecoder()
{
    avcodec_close(decoder_codec_context);
    avformat_close_input(&decoder_format_context);
    av_frame_free(&decoder_frame);
}

void ZeitEngine::Load(const QFileInfoList& sequence)
{
    // ATTENTION - might make sense to abstract this whole display init with InitDisplay() FreeDisplay() and such too
    display_initialized = false;

    FreeDecoder();
    FreeScaler();
    FreeFilter();
    FreeFilterData();
    FreeRescaler();

    source_sequence = sequence;

    if((*sequence.constBegin()).suffix() == "zd") {
        operation_mode = ZEIT_MODE_ZD;
    } else {
        operation_mode = ZEIT_MODE_GENERAL;
    }

    InitDecoder();

    Play();
}

void ZeitEngine::Refresh()
{
    preview_flag = true;

    control_mutex.lock();
    stop_flag = false;
    control_mutex.unlock();

    Play();
}

void ZeitEngine::Cache()
{
    for(sequence_iterator = source_sequence.constBegin(); sequence_iterator != source_sequence.constEnd(); ++sequence_iterator)
    {
        emit ProgressUpdated("Caching sequence", sequence_iterator - source_sequence.constBegin(), source_sequence.size());

        DecodeFrame();

        ScaleFrame(decoder_frame,
                   display_width,
                   display_height,
                   DISPLAY_AV_PIXEL_FORMAT);
    }

    emit ProgressUpdated("Cache ready", source_sequence.size(), source_sequence.size());
    emit MessageUpdated("Cache ready");

    FreeScaler();
}

void ZeitEngine::Play()
{
    control_mutex.lock();
    stop_flag = false;
    control_mutex.unlock();

    sequence_iterator = source_sequence.constBegin();

    if(!preview_flag) {
        emit MessageUpdated("Playback started");
    }

    while(true) {
        float frame_timeframe;

        timer.start();

        control_mutex.lock();
        bool break_requested = stop_flag || (!loop_flag && sequence_iterator == source_sequence.constEnd());

        switch(configured_framerate) {

            case ZEIT_RATE_23_976:
                frame_timeframe = 1000.0f / (30000.0f / 1001.0f);
                break;

            case ZEIT_RATE_29_97:
                frame_timeframe = 1000.0f / (2997.0f / 125.0f);
                break;

            default:
                frame_timeframe = 1000.0f / configured_framerate;
        }

        bool flip_x = flip_x_flag;
        bool flip_y = flip_y_flag;
        bool rotate_90d_cw = rotate_90d_cw_flag;

        ZeitFilter filter = filter_flag;

        control_mutex.unlock();

        if(break_requested) {
            emit MessageUpdated("Playback stopped");
            break;
        }

        if(sequence_iterator == source_sequence.constEnd()) {
            sequence_iterator = source_sequence.constBegin();
        }

        DecodeFrame();

        if(!display_initialized || (rotate_90d_cw != rotation_initialized)) {

            if(filter_initialized) {
                FreeFilterData();
                filter_initialized = false;
            }

            if(scaler_initialized) {
                FreeScaler();
                scaler_initialized = false;
            }

            // Swap sides for the fitting algorithm
            if(rotate_90d_cw) {
                display_width = decoder_frame->height;
                display_height = decoder_frame->width;
            } else {
                display_width = decoder_frame->width;
                display_height = decoder_frame->height;
            }

            // Fit display resolution into available screen space
            while(display_width > display_safe_max_width || display_height > display_safe_max_height) {

                if(display_width > display_safe_max_width) {
                    display_height = (float)display_height * ((float)display_safe_max_width / (float)display_width);
                    display_width = display_safe_max_width;
                }

                if(display_height > display_safe_max_height) {
                    display_width = (float)display_width * ((float)display_safe_max_height / (float)display_height);
                    display_height = display_safe_max_height;
                }
            }

            emit VideoConfigurationUpdated(display_width, display_height, DISPLAY_QT_PIXEL_FORMAT);

            // Pre-display everything is still internally unrotated, thus un-swap sides again!
            if(rotate_90d_cw) {
                int keep_width = display_width;
                display_width = display_height;
                display_height = keep_width;
            }

            rotation_initialized = rotate_90d_cw;

            while(!display_initialized) {

                Sleep(2);

                display->image_mutex.lock();
                display_initialized = (display->image != NULL);
                display->image_mutex.unlock();
            }
        }

        // extract size

        if(operation_mode == ZEIT_MODE_ZD) {
            DebayerFrame(decoder_frame, true);
            ScaleFrame(debayered_frame,
                       display_width,
                       display_height,
                       DISPLAY_AV_PIXEL_FORMAT);
        } else {
            ScaleFrame(decoder_frame,
                       display_width,
                       display_height,
                       DISPLAY_AV_PIXEL_FORMAT);
        }



        if(filter != ZEIT_FILTER_NONE) {
            FilterFrame(scaler_frame, filter);
        }

        display->image_mutex.lock();
        AVFrame* frame = (filter == ZEIT_FILTER_NONE ? scaler_frame :
                                                       filter_frame);

        for(int y = 0; y < frame->height; y++) {
            for(int x = 0; x < frame->width; x++) {
                int target_x = x;
                int target_y = y;

                if(flip_x) { target_x =  frame->width - 1 - x; }
                if(flip_y != rotate_90d_cw) { target_y = frame->height - 1 - y; }

                if(rotate_90d_cw) {
                    memcpy(display->image->scanLine(target_x) + target_y * 3,
                           frame->data[0] + y * frame->linesize[0] + x * 3,
                           sizeof(uint8_t) * 3);
                } else {
                    memcpy(display->image->scanLine(target_y) + target_x * 3,
                           frame->data[0] + y * frame->linesize[0] + x * 3,
                           sizeof(uint8_t) * 3);
                }
            }
        }
        display->image_mutex.unlock();

        emit VideoUpdated();

        FreeFilterData();

        ++sequence_iterator;

        if(timer.elapsed() > frame_timeframe) {
            // qDebug() << QString::number(timer.elapsed() - frame_timeframe) + "ms lag";
        } else {
            Sleep(frame_timeframe - timer.elapsed());
        }

        if(preview_flag) {
            preview_flag = false;
            break;
        }
    }

    FreeFilter();
    FreeScaler();
}

void ZeitEngine::Export(const QFileInfo file)
{
    sequence_iterator = source_sequence.constBegin();
    bool working;

    do {
        if(sequence_iterator != source_sequence.constEnd()) {
            emit ProgressUpdated("Encoding frames", sequence_iterator - source_sequence.constBegin(), source_sequence.size());

            control_mutex.lock();
            ZeitFilter filter = filter_flag;
            control_mutex.unlock();

            DecodeFrame();

            if(operation_mode == ZEIT_MODE_ZD) {
                DebayerFrame(decoder_frame, false);
                ScaleFrame(debayered_frame,
                           debayered_frame->width,
                           debayered_frame->height,
                           EXPORT_PIXELFORMAT);
            } else {
                ScaleFrame(decoder_frame,
                           decoder_frame->width,
                           decoder_frame->height,
                           EXPORT_PIXELFORMAT);
            }

            if(filter != ZEIT_FILTER_NONE) {
                FilterFrame(scaler_frame, filter);
                RescaleFrame(filter_frame,
                             filter_frame->width,
                             filter_frame->height,
                             EXPORT_PIXELFORMAT);

                working = ExportFrame(rescaler_frame, file);

                FreeFilterData();
            } else {
                working = ExportFrame(scaler_frame, file);
            }

            sequence_iterator++;
        } else {
            emit ProgressUpdated("Writing buffered frames", 0, 0);

            working = ExportFrame(NULL, file);
        }

    } while(working);

    emit ProgressUpdated("Encoding complete", source_sequence.size(), source_sequence.size());
    emit MessageUpdated("Encoding complete, finishing up export ...");

    CloseExport();
    FreeFilter();
    FreeScaler();
    FreeRescaler();

    emit MessageUpdated("Export complete");
}

void ZeitEngine::DecodeFrame()
{
    int frame_decoded, ret;

    try
    {
        // Necessary procedure to get a stable reference to a char* path representation - well, check again.
        QByteArray image_bytearray = (*sequence_iterator).absoluteFilePath().toUtf8();
        const char* image_cstr = image_bytearray.data();

        // Open input file
        if ((ret = avformat_open_input(&decoder_format_context, image_cstr, decoder_format, NULL)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to open input file '%s'\n", image_cstr);
            return;
        }

        // Allocate decoder packet
        decoder_packet = new AVPacket;
        av_init_packet(decoder_packet);

        // Read packet
        if( (ret = av_read_frame(decoder_format_context, decoder_packet)) < 0 ) {
            av_log(NULL, AV_LOG_ERROR, "Failed to read frame from file\n");
            throw(ret);
        }

        // Decode frame
        if( (ret = avcodec_decode_video2(decoder_codec_context, decoder_frame, &frame_decoded, decoder_packet)) < 0 || !frame_decoded ) {
            av_log(NULL, AV_LOG_ERROR, "Failed to decode image from file\n");
            throw(ret);
        }

        // Free decoder packet
        av_packet_unref(decoder_packet);

        // Convert deprecated pixel formats to favored ones
        // See http://stackoverflow.com/questions/23067722/swscaler-warning-deprecated-pixel-format-used
        switch (decoder_frame->format) {
        case AV_PIX_FMT_YUVJ420P:
            decoder_frame->format = AV_PIX_FMT_YUV420P;
            break;
        case AV_PIX_FMT_YUVJ422P:
            decoder_frame->format = AV_PIX_FMT_YUV422P;
            break;
        case AV_PIX_FMT_YUVJ444P:
            decoder_frame->format = AV_PIX_FMT_YUV444P;
            break;
        case AV_PIX_FMT_YUVJ440P:
            decoder_frame->format = AV_PIX_FMT_YUV440P;
        default:
            decoder_frame->format = (AVPixelFormat)decoder_frame->format;
            break;
        }
    }
    catch(int code) {
        av_log(NULL, AV_LOG_ERROR, "Return code was %d", code);
    }
}

void ZeitEngine::DebayerFrame(AVFrame *frame, bool fast_debayering)
{
    int ret;

    // Allocate debayer frame
    if( !(debayered_frame = av_frame_alloc()) ) {
        av_log(NULL, AV_LOG_ERROR, "Failed to allocate debayer frame\n");
        ret = AVERROR(ENOMEM);
        throw(ret);
    }

    av_frame_copy_props(debayered_frame, frame);
    debayered_frame->width = frame->width;
    debayered_frame->height = frame->height;
    debayered_frame->format = AV_PIX_FMT_RGB24;

    if( av_image_alloc(debayered_frame->data,
                       debayered_frame->linesize,
                       debayered_frame->width,
                       debayered_frame->height,
                       (AVPixelFormat)debayered_frame->format,
                       32) < 0 ) {
        fprintf(stderr, "Could not allocate debayer picture\n");
        exit(1);
    }

    for(int y = 0; y < frame->height; y++) {
        for(int x = 0; x < frame->width; x++) {

            int factor = 16; // 12bit (0-4096) to 8bit (0-256) range normalization factor

            uint8_t *source_pixel_ptr = frame->data[0] + (y * frame->linesize[0]) + (x * sizeof(uint16_t));
            uint8_t *target_pixel_ptr = debayered_frame->data[0] + (y * debayered_frame->linesize[0]) + (x * 3);

            uint8_t *target_pixel_red_ptr = target_pixel_ptr;
            uint8_t *target_pixel_green_ptr = target_pixel_ptr + sizeof(uint8_t);
            uint8_t *target_pixel_blue_ptr = target_pixel_ptr + 2 * sizeof(uint8_t);

            if(fast_debayering) {
                // Fast debayering using nearest neighbour estimation

                if(x % 2 == 0 && y % 2 == 0) {

                    // *  *  *
                    // * <G>[R]
                    // * [B] G

                    *target_pixel_red_ptr = *(uint16_t*)(source_pixel_ptr + sizeof(uint16_t)) / factor;
                    *target_pixel_green_ptr = *(uint16_t*)source_pixel_ptr / factor;
                    *target_pixel_blue_ptr = *(uint16_t*)(source_pixel_ptr + frame->linesize[0]) / factor;

                } else if(x % 2 == 1 && y % 2 == 1) {

                    //  G [R] *
                    // [B]<G> *
                    //  *  *  *

                    *target_pixel_red_ptr = *(uint16_t*)(source_pixel_ptr - frame->linesize[0]) / factor;
                    *target_pixel_green_ptr = *(uint16_t*)source_pixel_ptr / factor;
                    *target_pixel_blue_ptr = *(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) / factor;


                } else if(x % 2 == 1 && y % 2 == 0) {

                    //  *  *  *
                    //  * [G]<R>
                    //  * [B] G

                    *target_pixel_red_ptr = *(uint16_t*)source_pixel_ptr / factor;
                    *target_pixel_green_ptr = *(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) / factor;
                    *target_pixel_blue_ptr = *(uint16_t*)(source_pixel_ptr + frame->linesize[0] - sizeof(uint16_t)) / factor;

                } else if(x % 2 == 0 && y % 2 == 1) {

                    //  *  *  *
                    //  *  G [R]
                    //  * <B>[G]

                    *target_pixel_red_ptr = *(uint16_t*)(source_pixel_ptr - frame->linesize[0] + sizeof(uint16_t)) / factor;
                    *target_pixel_green_ptr = *(uint16_t*)(source_pixel_ptr + sizeof(uint16_t)) / factor;
                    *target_pixel_blue_ptr = *(uint16_t*)source_pixel_ptr / factor;

                }
            } else {
                // Slow but better debayering using bilinear filtering

                if(x % 2 == 0 && y % 2 == 0) {

                    if(x > 0) {
                        *target_pixel_red_ptr = ((*(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) +
                                                  *(uint16_t*)(source_pixel_ptr + sizeof(uint16_t))) / 2) / factor;
                    } else {
                        *target_pixel_red_ptr = *(uint16_t*)(source_pixel_ptr + sizeof(uint16_t)) / factor;
                    }

                    *target_pixel_green_ptr = *(uint16_t*)source_pixel_ptr / factor;

                    if(y > 0) {
                        *target_pixel_blue_ptr = ((*(uint16_t*)(source_pixel_ptr - frame->linesize[0]) +
                                                   *(uint16_t*)(source_pixel_ptr + frame->linesize[0])) / 2) / factor;
                    } else {
                        *target_pixel_blue_ptr = *(uint16_t*)(source_pixel_ptr + frame->linesize[0]) / factor;
                    }

                } else if(x % 2 == 1 && y % 2 == 1) {

                    if(y < frame->height - 1) {
                        *target_pixel_red_ptr = ((*(uint16_t*)(source_pixel_ptr - frame->linesize[0]) +
                                                  *(uint16_t*)(source_pixel_ptr + frame->linesize[0])) / 2) / factor;
                    } else {
                        *target_pixel_red_ptr = *(uint16_t*)(source_pixel_ptr - frame->linesize[0]) / factor;
                    }

                    *target_pixel_green_ptr = *(uint16_t*)source_pixel_ptr / factor;

                    if(x < frame->width - 1) {
                        *target_pixel_blue_ptr = ((*(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) +
                                                   *(uint16_t*)(source_pixel_ptr + sizeof(uint16_t))) / 2) / factor;
                    } else {
                        *target_pixel_blue_ptr = *(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) / factor;
                    }

                } else if(x % 2 == 1 && y % 2 == 0) {

                    *target_pixel_red_ptr = *(uint16_t*)source_pixel_ptr / factor;

                    if(y > 0) {
                        if(x < frame->width - 1) {
                            *target_pixel_green_ptr = ((*(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr + sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr - frame->linesize[0]) +
                                                        *(uint16_t*)(source_pixel_ptr + frame->linesize[0])) / 4) / factor;
                        } else {
                            *target_pixel_green_ptr = ((*(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr - frame->linesize[0]) +
                                                        *(uint16_t*)(source_pixel_ptr + frame->linesize[0])) / 3) / factor;
                        }
                    } else {
                        if(x < frame->width - 1) {
                            *target_pixel_green_ptr = ((*(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr + sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr + frame->linesize[0])) / 3) / factor;
                        } else {
                            *target_pixel_green_ptr = ((*(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr + frame->linesize[0])) / 2) / factor;
                        }
                    }

                    if(y > 0) {
                        if(x < frame->width - 1) {
                            *target_pixel_blue_ptr = ((*(uint16_t*)(source_pixel_ptr - frame->linesize[0] - sizeof(uint16_t)) +
                                                       *(uint16_t*)(source_pixel_ptr - frame->linesize[0] + sizeof(uint16_t)) +
                                                       *(uint16_t*)(source_pixel_ptr + frame->linesize[0] - sizeof(uint16_t)) +
                                                       *(uint16_t*)(source_pixel_ptr + frame->linesize[0] + sizeof(uint16_t))) / 4) / factor;
                        } else {
                            *target_pixel_blue_ptr = ((*(uint16_t*)(source_pixel_ptr - frame->linesize[0] - sizeof(uint16_t)) +
                                                       *(uint16_t*)(source_pixel_ptr + frame->linesize[0] - sizeof(uint16_t))) / 2) / factor;
                        }
                    } else {
                        if(x < frame->width - 1) {
                            *target_pixel_blue_ptr = ((*(uint16_t*)(source_pixel_ptr + frame->linesize[0] - sizeof(uint16_t)) +
                                                       *(uint16_t*)(source_pixel_ptr + frame->linesize[0] + sizeof(uint16_t))) / 2) / factor;
                        } else {
                            *target_pixel_blue_ptr = *(uint16_t*)(source_pixel_ptr + frame->linesize[0] - sizeof(uint16_t)) / factor;
                        }
                    }

                } else if(x % 2 == 0 && y % 2 == 1) {

                    if(x > 0) {
                        if(y < frame->height - 1) {
                            *target_pixel_red_ptr = ((*(uint16_t*)(source_pixel_ptr - frame->linesize[0] - sizeof(uint16_t)) +
                                                      *(uint16_t*)(source_pixel_ptr - frame->linesize[0] + sizeof(uint16_t)) +
                                                      *(uint16_t*)(source_pixel_ptr + frame->linesize[0] - sizeof(uint16_t)) +
                                                      *(uint16_t*)(source_pixel_ptr + frame->linesize[0] + sizeof(uint16_t))) / 4) / factor;
                        } else {
                            *target_pixel_red_ptr = ((*(uint16_t*)(source_pixel_ptr - frame->linesize[0] - sizeof(uint16_t)) +
                                                      *(uint16_t*)(source_pixel_ptr - frame->linesize[0] + sizeof(uint16_t))) / 2) / factor;
                        }
                    } else {
                        if(y < frame->height - 1) {
                            *target_pixel_red_ptr = ((*(uint16_t*)(source_pixel_ptr - frame->linesize[0] + sizeof(uint16_t)) +
                                                      *(uint16_t*)(source_pixel_ptr + frame->linesize[0] + sizeof(uint16_t))) / 2) / factor;
                        } else {
                            *target_pixel_red_ptr = *(uint16_t*)(source_pixel_ptr - frame->linesize[0] + sizeof(uint16_t)) / factor;
                        }
                    }

                    if(x > 0) {
                        if(y < frame->height - 1) {
                            *target_pixel_green_ptr = ((*(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr + sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr - frame->linesize[0]) +
                                                        *(uint16_t*)(source_pixel_ptr + frame->linesize[0])) / 4) / factor;
                        } else {
                            *target_pixel_green_ptr = ((*(uint16_t*)(source_pixel_ptr - sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr + sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr - frame->linesize[0])) / 3) / factor;
                        }
                    } else {
                        if(y < frame->height - 1) {
                            *target_pixel_green_ptr = ((*(uint16_t*)(source_pixel_ptr + sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr - frame->linesize[0]) +
                                                        *(uint16_t*)(source_pixel_ptr + frame->linesize[0])) / 3) / factor;
                        } else {
                            *target_pixel_green_ptr = ((*(uint16_t*)(source_pixel_ptr + sizeof(uint16_t)) +
                                                        *(uint16_t*)(source_pixel_ptr - frame->linesize[0])) / 2) / factor;
                        }
                    }

                    *target_pixel_blue_ptr = *(uint16_t*)source_pixel_ptr / factor;

                }
            }
        }
    }
}

void ZeitEngine::InitFilter(AVFrame* frame, ZeitFilter filter)
{
    AVFilter *buffersource = avfilter_get_by_name("buffer");
    AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *inputs = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVPixelFormat pix_fmts[] = { (AVPixelFormat)frame->format, AV_PIX_FMT_NONE };
    AVBufferSinkParams *buffersink_params;
    char args[512];
    int ret;

    try
    {
        filter_graph = avfilter_graph_alloc();

        // Buffer Video Source (receives decoded frames)
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                 frame->width,
                 frame->height,
                 frame->format,
                 1,
                 1,//source_framerate,//decoder_codec_context->time_base.den,
                 decoder_codec_context->sample_aspect_ratio.num,
                 decoder_codec_context->sample_aspect_ratio.den);

        if( (ret = avfilter_graph_create_filter(&buffersource_context,
                                                buffersource,
                                                "in",
                                                args,
                                                NULL,
                                                filter_graph)) < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
            throw(ret);
        }

        // Buffer Video Sink (terminates filter chain)
        buffersink_params = av_buffersink_params_alloc();
        buffersink_params->pixel_fmts = pix_fmts;
        ret = avfilter_graph_create_filter(&buffersink_context,
                                           buffersink,
                                           "out",
                                           NULL,
                                           buffersink_params,
                                           filter_graph);
        av_free(buffersink_params);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
            throw(ret);
        }

        // Filter graph endpoints
        outputs->name = av_strdup("in");
        outputs->filter_ctx = buffersource_context;
        outputs->pad_idx = 0;
        outputs->next = NULL;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = buffersink_context;
        inputs->pad_idx = 0;
        inputs->next = NULL;

        const char *filter_descriptor;

        switch(filter)
        {
        case ZEIT_FILTER_VIGNETTE:
            filter_descriptor = av_strdup("vignette=PI/4");
            break;
        case ZEIT_FILTER_BLACKWHITE:
            filter_descriptor = av_strdup("colorchannelmixer=.3:.4:.3:0:.3:.4:.3:0:.3:.4:.3");
            break;
        case ZEIT_FILTER_SEPIA:
            filter_descriptor = av_strdup("colorchannelmixer=.393:.769:.189:0:.349:.686:.168:0:.272:.534:.131");
            break;
        case ZEIT_FILTER_HIPSTAGRAM:
            filter_descriptor = av_strdup("colorbalance=rs=-.075:gs=.05:bs=.1:rm=.1:bm=-.05:rh=.1:gh=.1:bh=.1");
            break;
        case ZEIT_FILTER_NONE:
        default:
            filter_descriptor = av_strdup("null");
        }

        // Filter Danger Zone
        // const char *filter_descr = "boxblur=2:1:cr=0:ar=0";
        // const char *filter_descr = "scale=iw/1:-1";
        // const char *filter_descr = "zoompan=z='min(zoom+0.0015,1.5)':d=700:x='if(gte(zoom,1.5),x,x+1/a)':y='if(gte(zoom,1.5),y,y+1)':s=640x360";

        if( (ret = avfilter_graph_parse_ptr(filter_graph,
                                            filter_descriptor,
                                            &inputs,
                                            &outputs,
                                            NULL)) < 0)
        {
            throw(ret);
        }

        if( (ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        {
            return;
        }

        // Allocate filter frame
        if( !(filter_frame = av_frame_alloc()) ) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate filter frame\n");
            ret = AVERROR(ENOMEM);
            throw(ret);
        }

    }
    catch(int code)
    {
        av_log(NULL, AV_LOG_ERROR, "Return code was %d", code);
        return;
    }

    configured_filter = filter;
    filter_initialized = true;
}

void ZeitEngine::FilterFrame(AVFrame* frame, ZeitFilter filter)
{
    if(!filter_initialized) {
        InitFilter(frame, filter);
    } else if(filter != configured_filter) {
        FreeFilter();
        InitFilter(frame, filter);
    }

    int ret;

    // Push the decoded frame into the filtergraph
    if( (ret = av_buffersrc_add_frame_flags(buffersource_context, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
        throw(ret);
    }

    // Pull filtered frames from the filtergraph
    while(true) {
        ret = av_buffersink_get_frame(buffersink_context, filter_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) { throw(ret); }
    }
}

void ZeitEngine::FreeFilterData()
{
    if(filter_initialized) {
        av_frame_unref(filter_frame);
    }
}

void ZeitEngine::FreeFilter()
{
    if(filter_initialized) {
        avfilter_graph_free(&filter_graph);

        filter_initialized = false;
        configured_filter = ZEIT_FILTER_NONE;
    }
}

void ZeitEngine::InitScaler(AVFrame *frame,
                            const unsigned int target_width,
                            const unsigned int target_height,
                            const AVPixelFormat target_pixel_format)
{
    int ret;

    try
    {
        if( !(scaler_context = sws_getContext(frame->width,
                                              frame->height,
                                              (AVPixelFormat)frame->format,
                                              target_width,
                                              target_height,
                                              target_pixel_format,
                                              SWS_BILINEAR,
                                              NULL,
                                              NULL,
                                              NULL)) )
        {
            av_log(NULL, AV_LOG_ERROR,
            "Impossible to create scale context for the conversion "
            "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
            av_get_pix_fmt_name((AVPixelFormat)frame->format), frame->width, frame->height,
            av_get_pix_fmt_name(target_pixel_format), target_width, target_height);
            ret = AVERROR(EINVAL);
            throw(ret);
        }

        // Allocate scaler frame
        if( !(scaler_frame = av_frame_alloc()) ) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate scaler frame\n");
            ret = AVERROR(ENOMEM);
            throw(ret);
        }

        av_frame_copy_props(scaler_frame, frame);
        scaler_frame->width = target_width;
        scaler_frame->height = target_height;
        scaler_frame->format = target_pixel_format;

        // Alignment has to be 32 because QImage needs it that way - But we don't always write to QImage - Keep an eye on this
        if( av_image_alloc(scaler_frame->data,
                           scaler_frame->linesize,
                           scaler_frame->width,
                           scaler_frame->height,
                           (AVPixelFormat)scaler_frame->format,
                           32) < 0 ) {
            fprintf(stderr, "Could not allocate scaler picture\n");
            exit(1);
        }

        // Buffer is going to be written to rawvideo file, no alignment <-- hm!
        //        if ((ret = av_image_alloc(scaler_data,
        //                                  scaler_linesize,
        //                                  target_width,
        //                                  target_height,
        //                                  target_pixel_format,
        //                                  1)) < 0)
        //        {
        //            fprintf(stderr, "Could not allocate destination image\n");
        //            throw(ret);
        //        }

        scaler_initialized = true;
    }
    catch(int code)
    {
        av_log(NULL, AV_LOG_ERROR, "Return code was %d", code);
        return;
    }
}

void ZeitEngine::ScaleFrame(AVFrame *frame,
                            const unsigned int target_width,
                            const unsigned int target_height,
                            const AVPixelFormat target_pixel_format)
{
    if(!scaler_initialized) {
        InitScaler(frame,
                   target_width,
                   target_height,
                   target_pixel_format);
    }

    sws_scale(scaler_context,
              (const uint8_t * const*)frame->data,
              frame->linesize,
              0,
              frame->height,
              scaler_frame->data,
              scaler_frame->linesize);
}

void ZeitEngine::FreeScaler()
{
    if(scaler_initialized) {
        sws_freeContext(scaler_context);
        av_freep(&scaler_frame->data[0]);
        av_frame_free(&scaler_frame);

        scaler_initialized = false;
    }
}

void ZeitEngine::InitRescaler(AVFrame *frame,
                              const unsigned int target_width,
                              const unsigned int target_height,
                              const AVPixelFormat target_pixel_format)
{
    int ret;

    try
    {
        if( !(rescaler_context = sws_getContext(frame->width,
                                                frame->height,
                                                (AVPixelFormat)frame->format,
                                                target_width,
                                                target_height,
                                                target_pixel_format,
                                                SWS_BILINEAR,
                                                NULL,
                                                NULL,
                                                NULL)) )
        {
            av_log(NULL, AV_LOG_ERROR,
            "Impossible to create scale context for the conversion "
            "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
            av_get_pix_fmt_name((AVPixelFormat)frame->format), frame->width, frame->height,
            av_get_pix_fmt_name(target_pixel_format), target_width, target_height);
            ret = AVERROR(EINVAL);
            throw(ret);
        }

        // Allocate scaler frame
        if( !(rescaler_frame = av_frame_alloc()) ) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate scaler frame\n");
            ret = AVERROR(ENOMEM);
            throw(ret);
        }

        av_frame_copy_props(rescaler_frame, frame);
        rescaler_frame->width = target_width;
        rescaler_frame->height = target_height;
        rescaler_frame->format = target_pixel_format;

        // Alignment has to be 32 because QImage needs it that way - But we don't always write to QImage - Keep an eye on this
        if( av_image_alloc(rescaler_frame->data,
                           rescaler_frame->linesize,
                           rescaler_frame->width,
                           rescaler_frame->height,
                           (AVPixelFormat)rescaler_frame->format,
                           32) < 0 ) {
            fprintf(stderr, "Could not allocate scaler picture\n");
            exit(1);
        }

        // Buffer is going to be written to rawvideo file, no alignment <-- hm!
        //        if ((ret = av_image_alloc(scaler_data,
        //                                  scaler_linesize,
        //                                  target_width,
        //                                  target_height,
        //                                  target_pixel_format,
        //                              `    1)) < 0)
        //        {
        //            fprintf(stderr, "Could not allocate destination image\n");
        //            throw(ret);
        //        }

        rescaler_initialized = true;
    }
    catch(int code)
    {
        av_log(NULL, AV_LOG_ERROR, "Return code was %d", code);
        return;
    }
}

void ZeitEngine::RescaleFrame(AVFrame *frame,
                              const unsigned int target_width,
                              const unsigned int target_height,
                              const AVPixelFormat target_pixel_format)
{
    if(!rescaler_initialized) {
        InitRescaler(frame,
                     target_width,
                     target_height,
                     target_pixel_format);
    }

    sws_scale(rescaler_context,
              (const uint8_t * const*)frame->data,
              frame->linesize,
              0,
              frame->height,
              rescaler_frame->data,
              rescaler_frame->linesize);
}

void ZeitEngine::FreeRescaler()
{
    if(rescaler_initialized) {
        sws_freeContext(rescaler_context);
        av_freep(&rescaler_frame->data[0]);
        av_frame_free(&rescaler_frame);

        rescaler_initialized = false;
    }
}

void ZeitEngine::InitExporter(AVFrame* frame, const QFileInfo output_file)
{
    int ret;

    try {
        avformat_alloc_output_context2(&output_format_context,
                                       NULL,
                                       NULL,
                                       output_file.absoluteFilePath().toUtf8().data());

        if(!output_format_context) {
            throw("Could not allocate output context");
        }

        output_format = output_format_context->oformat;

//        encoder_output_file = new std::ofstream;

        output_codec = avcodec_find_encoder(EXPORT_CODEC_ID);

        if(!output_codec) {
            throw("Output codec not found");
        }

        output_stream = avformat_new_stream(output_format_context, output_codec);

        if(!output_stream) {
            throw("Could not allocate output stream");
        }

        output_stream->id = output_format_context->nb_streams - 1;

        output_codec_context = output_stream->codec;

        output_codec_context->codec_id = output_format->video_codec;
        output_codec_context->bit_rate = 25000000;

//        output_codec_context = avcodec_alloc_context3(output_encoder)
//        if(!output_codec_context) {
//            throw("Could not allocate video codec context");
//        }

        control_mutex.lock();
        if(rotate_90d_cw_flag) {
            output_codec_context->width = frame->height;
            output_codec_context->height = frame->width;
        } else {
            output_codec_context->width = frame->width;
            output_codec_context->height = frame->height;
        }
        control_mutex.unlock();

        switch(configured_framerate) {

            case ZEIT_RATE_23_976:
                output_stream->time_base.num = 1001;
                output_stream->time_base.den = 30000;
                break;

            case ZEIT_RATE_29_97:
                output_stream->time_base.num = 125;
                output_stream->time_base.den = 2997;
                break;

            default:
                output_stream->time_base.num = 1;
                output_stream->time_base.den = configured_framerate;
                break;
        }

        output_codec_context->time_base = output_stream->time_base;
        output_codec_context->gop_size = configured_framerate;
        output_codec_context->pix_fmt = EXPORT_PIXELFORMAT;

//        if(EXPORT_CODEC_ID == AV_CODEC_ID_H264) {
//            /* ??? Not from muxing.c reference, maybe outdated or wrong */
//            av_opt_set(output_codec_context->priv_data, "preset", "slow", 0);
//        }

        /* Some formats want stream headers to be separate. */
        if(output_format_context->oformat->flags & AVFMT_GLOBALHEADER) {
            output_codec_context->flags |= CODEC_FLAG_GLOBAL_HEADER;
        }

        output_options = NULL;

        ret = avcodec_open2(output_codec_context, output_codec, &output_options);

        av_dict_free(&output_options);

        if(ret < 0) {
            throw("Could not open output video codec");
        }

//        // Open encoder codec
//        if( avcodec_open2(output_codec_context, output_codec, NULL) < 0 ) {
//            av_log(NULL, AV_LOG_ERROR, "Could not open codec\n");
//            exit(1);
//        }

//        encoder_output_file->open(output_file.absoluteFilePath().toUtf8().data(), std::ios::out | std::ios::binary);
//        if ( !encoder_output_file->is_open() ) {
//            av_log(NULL, AV_LOG_ERROR, "Could not open input file '%s'\n", output_file.absoluteFilePath().toUtf8().data());
//            exit(1);
//        }

        // HEADS UP: muxing.c uses AVPicture !!!
        output_frame = av_frame_alloc();

        if (!output_frame) {
            throw("Could not allocate video output frame");
        }

        output_frame->format = output_codec_context->pix_fmt;
        output_frame->width = output_codec_context->width;
        output_frame->height = output_codec_context->height;

        // The image can be allocated by any means and av_image_alloc() is just the most convenient way if av_malloc() is to be used
        if( (ret = av_image_alloc(output_frame->data, output_frame->linesize, output_codec_context->width, output_codec_context->height, output_codec_context->pix_fmt, 1)) < 0) {
            fprintf(stderr, "Could not allocate raw picture buffer\n");
            exit(1);
        }
        // END HEADS UP

        if(!(output_format->flags & AVFMT_NOFILE)) {
            ret = avio_open(&output_format_context->pb,
                            output_file.absoluteFilePath().toUtf8().data(),
                            AVIO_FLAG_WRITE);

            if(ret < 0) {
                throw("Could not open output file");
            }
        }

        ret = avformat_write_header(output_format_context, &output_options);

        if(ret < 0) {
            throw("An error occured during writing the stream header");
        }

        // Possibly elsewhere, legacy code: encoder_packet = new AVPacket;
    }
    catch(...) {
        return;
    }
}

bool ZeitEngine::ExportFrame(AVFrame* frame, const QFileInfo output_file)
{
    int got_output_packet = 0;
    int ret;

    if(!exporter_initialized) {
        InitExporter(frame, output_file);
        exporter_initialized = true;
    }

    // Copy to output frame only until we're writing the delayed frames
    if(frame != NULL) {

      control_mutex.lock();
      bool flip_x = flip_x_flag;
      bool flip_y = flip_y_flag;
      bool rotate_90d_cw = rotate_90d_cw_flag;
      control_mutex.unlock();

      // Y
      for(int y = 0; y < frame->height; y++) {
          for(int x = 0; x < frame->width; x++) {
              int target_x = x;
              int target_y = y;

              if(flip_x) { target_x = frame->width - 1 - x; }
              if(flip_y != rotate_90d_cw) { target_y = frame->height - 1 - y; }

              if(rotate_90d_cw) {
                  memcpy(output_frame->data[0] + target_x * output_frame->linesize[0] + target_y,
                         frame->data[0] + y * frame->linesize[0] + x,
                         sizeof(uint8_t));
              } else {
                  memcpy(output_frame->data[0] + target_y * output_frame->linesize[0] + target_x,
                         frame->data[0] + y * frame->linesize[0] + x,
                         sizeof(uint8_t));
              }
          }
      }

      // Cb and Cr
      for(int y = 0; y < frame->height / 2; y++) {
          for(int x = 0; x < frame->width / 2; x++) {
              int target_x = x;
              int target_y = y;

              if(flip_x) { target_x =  frame->width / 2 - 1 - x; }
              if(flip_y != rotate_90d_cw) { target_y = frame->height / 2 - 1 - y; }

              // (Iterate over Cb and Cr Plane)
              for(int p = 1; p < 3; p++) {
                  if(rotate_90d_cw) {
                      memcpy(output_frame->data[p] + target_x * output_frame->linesize[p] + target_y,
                             frame->data[p] + y * frame->linesize[p] + x,
                             sizeof(uint8_t));
                  } else {
                      memcpy(output_frame->data[p] + target_y * output_frame->linesize[p] + target_x,
                             frame->data[p] + y * frame->linesize[p] + x,
                             sizeof(uint8_t));
                  }
              }
          }
      }

      output_frame->pts = sequence_iterator - source_sequence.constBegin();
    }

    AVPacket output_packet;
    av_init_packet(&output_packet);

    ret = avcodec_encode_video2(output_codec_context,
                                &output_packet,
                                frame ? output_frame : NULL,
                                &got_output_packet);

    if(ret < 0) {
        throw("Error encoding output video frame");
    }

    if(got_output_packet) {
        av_packet_rescale_ts(&output_packet,
                             output_codec_context->time_base,
                             output_stream->time_base);

        output_packet.stream_index = output_stream->index;

        ret = av_interleaved_write_frame(output_format_context,
                                         &output_packet);
    } else {
        ret = 0;
    }

    if(ret < 0) {
        char t[1024];
        av_make_error_string(t, 1024, ret);

        throw("Error while writing output video frame");
    }

    return frame || got_output_packet;

}

void ZeitEngine::CloseExport()
{
    av_write_trailer(output_format_context);

    avcodec_close(output_codec_context);

    if(!(output_format->flags & AVFMT_NOFILE)) {
        avio_closep(&output_format_context->pb);
    }

    avformat_free_context(output_format_context);

    av_freep(&output_frame->data[0]);
    av_frame_free(&output_frame);

    exporter_initialized = false;

    ControlsEnabled(true);
}

void ZeitEngine::Sleep(const unsigned int msec) {

    QMutex wait_mutex;

    wait_mutex.lock();

    QWaitCondition wait_condition;
    wait_condition.wait(&wait_mutex, msec);

    wait_mutex.unlock();

}
