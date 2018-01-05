typedef struct VAAPIVPPContext {
    AVVAAPIDeviceContext *hwctx;
    AVBufferRef *device_ref;

    int valid_ids;
    VAConfigID  va_config;
    VAContextID va_context;

    AVBufferRef       *input_frames_ref;
    AVHWFramesContext *input_frames;

    AVBufferRef       *output_frames_ref;
    AVHWFramesContext *output_frames;

    enum AVPixelFormat output_format;
    int output_width;
    int output_height;

    VABufferID         filter_buffer;
} VAAPIVPPContext;

int vaapi_vpp_query_formats(AVFilterContext *avctx);

int vaapi_vpp_pipeline_uninit(ScaleVAAPIContext *ctx); /* No need */

int vaapi_vpp_config_input(AVFilterLink *inlink);

int vaapi_vpp_config_output(AVFilterLink *outlink);

int vaapi_vpp_colour_standard(enum AVColorSpace av_cs);

int vaapi_vpp_render_picture(ctx, params, input_frame, output_frame);

int vaapi_vpp_init(AVFilterContext *avctx);

void vaapi_vpp_uninit(AVFilterContext *avctx);

