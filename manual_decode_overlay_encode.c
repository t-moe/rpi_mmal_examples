#include "bcm_host.h"
#include "mmal.h"
#include "mmal_port.h"
#include "mmal_pool.h"
#include "util/mmal_connection.h"
#include "util/mmal_default_components.h"
#include "util/mmal_util.h"
#include "util/mmal_util_params.h"
#include <stdio.h>
#include "interface/vcos/vcos.h"


#include<arpa/inet.h>
#include<sys/socket.h>

#define CHECK_STATUS(status, msg) if (status != MMAL_SUCCESS) { fprintf(stderr, msg"\n"); goto error; }

static FILE *source_file;
static FILE *dest_file;

/* Macros abstracting the I/O, just to make the example code clearer */


#define SOURCE_OPEN(uri) \
    source_file = fopen(uri, "rb"); if (!source_file) goto error;
#define SOURCE_READ_DATA_INTO_BUFFER(a) \
    a->length = fread(a->data, 1, a->alloc_size - 128, source_file); \
    a->offset = 0; a->pts = a->dts = MMAL_TIME_UNKNOWN
#define SOURCE_CLOSE() \
    if (source_file) fclose(source_file)

#define DEST_OPEN(uri) \
    dest_file = fopen(uri, "wb"); if (!dest_file) goto error;
#define DEST_WRITE_DATA_INTO_FILE(buf,len) \
    fwrite(buf,1,len,dest_file)
#define DEST_CLOSE() \
    if (dest_file) fclose(dest_file)

/** Context for our application */
static struct CONTEXT_T {
    VCOS_SEMAPHORE_T semaphore;
    MMAL_QUEUE_T *queue_encoded;
    MMAL_PORT_T* encoder_input_port;
    MMAL_PORT_T* encoder_output_port;
    MMAL_POOL_T * encoder_pool_in;
    MMAL_STATUS_T status;
} context;


static void log_video_format(MMAL_ES_FORMAT_T *format)
{
    if (format->type != MMAL_ES_TYPE_VIDEO)
        return;

    fprintf(stderr, "fourcc: %s, variant; %s width: %i, height: %i, (%i,%i,%i,%i)\n",
            (char *)&format->encoding,
            (char *)&format->encoding_variant,
            format->es->video.width, format->es->video.height,
            format->es->video.crop.x, format->es->video.crop.y,
            format->es->video.crop.width, format->es->video.crop.height);
}


/** Callback from the encoder input port.
 * Buffer has been consumed and is available to be used again. */
static void encoder_input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

    /* The encoder is done with the data, just recycle the buffer header into its pool */
    mmal_buffer_header_release(buffer);

    /* Kick the processing thread */
   // vcos_semaphore_post(&ctx->semaphore);

    //fprintf(stderr,"encoder input callback\n");
}


/** Callback from the encoder output port.
 * Buffer has been produced by the port and is available for processing. */
static void encoder_output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

    /* Queue the encoded video frame so that we can grab it in the main loop*/
    mmal_queue_put(ctx->queue_encoded, buffer);

    /* Kick the processing thread */
    vcos_semaphore_post(&ctx->semaphore);

    //fprintf(stderr,"encoder output callback\n");
}



/** Callback from the decoder input port.
 * Buffer has been consumed and is available to be used again. */
static void decoder_input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

    /* The decoder is done with the data, just recycle the buffer header into its pool */
    mmal_buffer_header_release(buffer);

    /* Kick the processing thread */
    vcos_semaphore_post(&ctx->semaphore);

    //fprintf(stderr,"decoder input callback\n");
}



/** Callback from the pool. Buffer is available. */
static MMAL_BOOL_T pool_buffer_available_callback(MMAL_POOL_T *pool, MMAL_BUFFER_HEADER_T *buffer,
   void *userdata)
{
   MMAL_PARAM_UNUSED(userdata);

   mmal_queue_put(pool->queue, buffer);

   return 0;
}


/** Callback from the decoder output port.
 * Buffer has been produced by the port and is available for processing. */
static void decoder_output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

    if (buffer->cmd) {
        if (buffer->cmd != MMAL_EVENT_FORMAT_CHANGED) {
            fprintf(stderr,"unknown cmd: %u\n",buffer->cmd);
            return;
        }

        MMAL_EVENT_FORMAT_CHANGED_T *event = mmal_event_format_changed_get(buffer);
        if (!event){
            fprintf(stderr,"could not get format change event");
            return;
        }

        MMAL_STATUS_T status = mmal_format_full_copy(ctx->encoder_input_port->format, event->format);
        if (status != MMAL_SUCCESS) {
            fprintf(stderr,"could not copy format from event to input format: %s",mmal_status_to_string(status));
            return;
        }
        status = mmal_port_format_commit(ctx->encoder_input_port);
        if (status != MMAL_SUCCESS) {
          fprintf(stderr,"could not commit input format: %s",mmal_status_to_string(status));
          return;
        }

        status = mmal_port_enable(ctx->encoder_input_port, encoder_input_callback);
        if (status != MMAL_SUCCESS) {
          fprintf(stderr,"could not enable encoder input port: %s",mmal_status_to_string(status));
          return;
        }

        status = mmal_port_enable(ctx->encoder_output_port, encoder_output_callback);
        if (status != MMAL_SUCCESS) {
          fprintf(stderr,"could not enable encoder output port: %s",mmal_status_to_string(status));
          return;
        }

        //create pool with corret buffer requirements
        ctx->encoder_pool_in =mmal_port_pool_create(ctx->encoder_input_port,ctx->encoder_input_port->buffer_num_recommended,ctx->encoder_input_port->buffer_size_recommended);
        mmal_pool_callback_set(ctx->encoder_pool_in, pool_buffer_available_callback, NULL);

        mmal_buffer_header_release(buffer);
        fprintf(stderr,"Encoder enabled\n");

    } else {

        ctx->status = mmal_port_send_buffer(ctx->encoder_input_port, buffer);
        if (ctx->status != MMAL_SUCCESS)
        {
            fprintf(stderr,"could not send buffer from decoder output to encoder input: %s\n",
                    mmal_status_to_string(ctx->status));
            mmal_buffer_header_release(buffer);
            //mmal_event_error_send(connection->out->component, status);
        }
    }


    /* Kick the processing thread */
    //vcos_semaphore_post(&ctx->semaphore);

    //fprintf(stderr,"decoder output callback\n");
}




/** Callback from the control port.
 * Component is sending us an event. */
static void decoder_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

    switch (buffer->cmd)
    {
    case MMAL_EVENT_EOS:
        /* Only sink component generate EOS events */
        break;
    case MMAL_EVENT_ERROR:
        /* Something went wrong. Signal this to the application */
        ctx->status = *(MMAL_STATUS_T *)buffer->data;
        break;
    default:
        break;
    }

    /* Done with the event, recycle it */
    mmal_buffer_header_release(buffer);

    fprintf(stderr,"control cb. status %s\n",mmal_status_to_string(ctx->status));
}


int main(int argc, char* argv[]) {

    MMAL_STATUS_T status;
    //MMAL_CONNECTION_T *conn = NULL;
    MMAL_COMPONENT_T *decoder = NULL, *encoder=NULL;
    MMAL_POOL_T *decoder_pool_in = NULL, *encoder_pool_out = NULL;
    MMAL_ES_FORMAT_T * format_in=NULL ;
    MMAL_BOOL_T eos_sent = MMAL_FALSE, eos_received;
    MMAL_BUFFER_HEADER_T *buffer;
    int framenr=0;

    bcm_host_init();
    vcos_semaphore_create(&context.semaphore, "example", 1);

    SOURCE_OPEN("test.h264_2")
    DEST_OPEN("out.h264")


    /* Create the components */
    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, &decoder);
    CHECK_STATUS(status, "failed to create decoder");

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);
    CHECK_STATUS(status, "failed to create encoder");



    /* Enable control port so we can receive events from the component */
    decoder->control->userdata = (struct MMAL_PORT_USERDATA_T*)(void *)&context;
    status = mmal_port_enable(decoder->control, decoder_control_callback);
    CHECK_STATUS(status, "failed to enable control port");


    /* Enable zero-copy parameters on all ports */
    status = mmal_port_parameter_set_boolean(decoder->input[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    CHECK_STATUS(status, "failed to set zero copy on decoder input");
    status = mmal_port_parameter_set_boolean(decoder->output[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    CHECK_STATUS(status, "failed to set zero copy on decoder output");
    status = mmal_port_parameter_set_boolean(encoder->input[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    CHECK_STATUS(status, "failed to set zero copy on encoder input");
    status = mmal_port_parameter_set_boolean(encoder->output[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    CHECK_STATUS(status, "failed to set zero copy on encoder output");


    /* Set format of video decoder input port */
    format_in = decoder->input[0]->format;
    format_in->type = MMAL_ES_TYPE_VIDEO;
    format_in->encoding = MMAL_ENCODING_H264;
    format_in->es->video.width = 1280;
    format_in->es->video.height = 720;
    format_in->es->video.frame_rate.num = 25;
    format_in->es->video.frame_rate.den = 1;
    format_in->es->video.par.num = 1;
    format_in->es->video.par.den = 1;
    /* If the data is known to be framed then the following flag should be set:*/
    //format_in->flags |= MMAL_ES_FORMAT_FLAG_FRAMED;


    status = mmal_port_format_commit(decoder->input[0]);
    CHECK_STATUS(status, "failed to commit format");

    //format_out = decoder->output[0]->format;
    //status = mmal_port_format_commit(decoder->output[0]);
    //CHECK_STATUS(status, "failed to commit format");

    decoder->input[0]->buffer_num = decoder->input[0]->buffer_num_min;
    decoder->input[0]->buffer_size = decoder->input[0]->buffer_size_min;

    decoder->output[0]->buffer_num = decoder->output[0]->buffer_num_min;
    decoder->output[0]->buffer_size = decoder->output[0]->buffer_size_min;

    decoder_pool_in = mmal_port_pool_create(decoder->input[0],decoder->input[0]->buffer_num,decoder->input[0]->buffer_size);
    encoder_pool_out = mmal_port_pool_create(encoder->output[0], encoder->output[0]->buffer_num, encoder->output[0]->buffer_size);


    context.queue_encoded = mmal_queue_create();
    context.encoder_pool_in = NULL; //pool cannot be allocated yet, because buffer requirements are not known yet. See decoder output callback
    context.encoder_input_port = encoder->input[0];
    context.encoder_output_port = encoder->output[0];

    /* Store a reference to our context in each port (will be used during callbacks) */
    decoder->input[0]->userdata = (void *)&context;
    decoder->output[0]->userdata = (void*) &context;
    encoder->input[0]->userdata = (void*) &context;
    encoder->output[0]->userdata = (void *)&context;

    status = mmal_port_enable(decoder->input[0], decoder_input_callback);
    CHECK_STATUS(status, "failed to enable decoder input port")

    status = mmal_port_enable(decoder->output[0], decoder_output_callback);
    CHECK_STATUS(status, "failed to enable decoder output port")


    //we do not enable the encoder ports yet. Instead we wait on the format change event on the decoder output. Then we finish configuring the encoder.


    /* Start transcoding */
    fprintf(stderr, "start transcoding\n");

    while(eos_received == MMAL_FALSE)
    {
        /* Wait for buffer headers to be available on either the decoder input or the encoder output port */
        vcos_semaphore_wait(&context.semaphore);

        /* Send data to decode to the input port of the video decoder */
        if ((buffer = mmal_queue_get(decoder_pool_in->queue)) != NULL) //Get empty buffers from queue
        {
            SOURCE_READ_DATA_INTO_BUFFER(buffer);
            if(!buffer->length) eos_sent = MMAL_TRUE;

            buffer->flags = buffer->length ? 0 : MMAL_BUFFER_HEADER_FLAG_EOS;
            buffer->pts = buffer->dts = MMAL_TIME_UNKNOWN;
            //fprintf(stderr, "sending %i bytes\n", (int)buffer->length);
            status = mmal_port_send_buffer(decoder->input[0], buffer);
            CHECK_STATUS(status, "failed to send buffer");
        }


        /* receive encoded frames and store them */
        while ((buffer = mmal_queue_get(context.queue_encoded)) != NULL)
        {
            /* We have a frame, do something with it
                * Once we're done with it, we release it. It will automatically go back
                * to its original pool so it can be reused for a new video frame.
                */
            eos_received = buffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS;

            if (buffer->cmd)
            {
                fprintf(stderr, "received event %4.4s\n", (char *)&buffer->cmd);
            }
            else
            {
                DEST_WRITE_DATA_INTO_FILE(buffer->data, buffer->length);
                fprintf(stderr, "encoded frame %u (flags %x, length %u)\n",framenr++, buffer->flags, buffer->length);
            }
            mmal_buffer_header_release(buffer);
        }

        if(encoder->output[0]->is_enabled) { //do not send buffers until all ports and pools are created

            /* Send empty buffers to the output port of the decoder */
            while((buffer = mmal_queue_get(context.encoder_pool_in->queue)) != NULL)
            {
                status =  mmal_port_send_buffer(decoder->output[0], buffer);
                CHECK_STATUS(status, "failed to send empty buffer to decoder output\n");
            }

            /* Send empty buffers to the output port of the encoder */
            while ((buffer = mmal_queue_get(encoder_pool_out->queue)) != NULL)
            {
                status = mmal_port_send_buffer(encoder->output[0], buffer);
                CHECK_STATUS(status, "failed to send empty buffer to encoder output\n");
            }
        }


    }

    /* Stop decoding */
    fprintf(stderr, "stop transcoding\n");

    /* Stop everything. Not strictly necessary since mmal_component_destroy()
       * will do that anyway */

    mmal_port_disable(decoder->input[0]);
    mmal_port_disable(decoder->control);
    mmal_port_disable(encoder->input[0]);
    mmal_port_disable(decoder->output[0]);
    mmal_port_disable(encoder->output[0]);
    fprintf(stderr, "done\n");

    SOURCE_CLOSE();
    DEST_CLOSE();

error:
    /* Cleanup everything */
    if (decoder)
        mmal_component_release(decoder);
    if (encoder)
        mmal_component_release(encoder);

    return status == MMAL_SUCCESS ? 0 : -1;

}
