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
    MMAL_QUEUE_T *queue;
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


/** Callback from the decoder input port.
 * Buffer has been consumed and is available to be used again. */
static void input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

    /* The decoder is done with the data, just recycle the buffer header into its pool */
    mmal_buffer_header_release(buffer);

    /* Kick the processing thread */
    vcos_semaphore_post(&ctx->semaphore);

    //fprintf(stderr,"decoder input callback\n");
}


/** Callback from the encoder output port.
 * Buffer has been produced by the port and is available for processing. */
static void output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

    /* Queue the encoded video frame */
    mmal_queue_put(ctx->queue, buffer);

    /* Kick the processing thread */
    vcos_semaphore_post(&ctx->semaphore);

    //fprintf(stderr,"encoder output callback\n");
}


/** Callback from the control port.
 * Component is sending us an event. */
static void control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
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

   fprintf(stderr,"control cb. status %u\n", ctx->status);
}

int main(int argc, char* argv[]) {

    MMAL_STATUS_T status;
    MMAL_CONNECTION_T *conn = NULL;
    MMAL_COMPONENT_T *decoder = 0, *encoder=0;
    MMAL_POOL_T *pool_in = NULL, *pool_out = NULL;
    MMAL_ES_FORMAT_T * format_in=0, *format_out=0;
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
    status = mmal_port_enable(decoder->control, control_callback);
    CHECK_STATUS(status, "failed to enable control port");


    /* Set the zero-copy parameter on the input port */
    mmal_port_parameter_set_boolean(decoder->input[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    CHECK_STATUS(status, "failed to set zero copy on decoder input");

    /* Set the zero-copy parameter on the output port */
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


    format_out = decoder->output[0]->format;

    status = mmal_port_format_commit(decoder->output[0]);
    CHECK_STATUS(status, "failed to commit format");

    decoder->input[0]->buffer_num = decoder->input[0]->buffer_num_min;
    decoder->input[0]->buffer_size = decoder->input[0]->buffer_size_min;

    decoder->output[0]->buffer_num = decoder->output[0]->buffer_num_min;
    decoder->output[0]->buffer_size = decoder->output[0]->buffer_size_min;

    pool_in = mmal_port_pool_create(decoder->input[0],decoder->input[0]->buffer_num,decoder->input[0]->buffer_size);

    context.queue = mmal_queue_create();

    /* Store a reference to our context in each port (will be used during callbacks) */
    decoder->input[0]->userdata = (struct MMAL_PORT_USERDATA_T*)(void *)&context;
    encoder->output[0]->userdata =  (struct MMAL_PORT_USERDATA_T*)(void *)&context;

    status = mmal_port_enable(decoder->input[0], input_callback);
    CHECK_STATUS(status, "failed to enable input port")



    /* connect them up - this propagates port settings from outputs to inputs */
    status = mmal_connection_create(&conn, decoder->output[0], encoder->input[0], MMAL_CONNECTION_FLAG_TUNNELLING);
    log_video_format(encoder->input[0]->format);
    CHECK_STATUS(status, "failed to connect decoder to encoder")


     /* Start playback */
    fprintf(stderr, "start");
    status = mmal_connection_enable(conn);
    CHECK_STATUS(status, "failed to enable connection");

    status = mmal_port_enable(encoder->output[0], output_callback);
    CHECK_STATUS(status, "failed to enable output port");

    pool_out = mmal_port_pool_create(encoder->output[0], encoder->output[0]->buffer_num, encoder->output[0]->buffer_size);
    while((buffer = mmal_queue_get(pool_out->queue)) != NULL)
    {
        mmal_port_send_buffer(encoder->output[0], buffer);
    }

    while(eos_received == MMAL_FALSE)
    {
        /* Wait for buffer headers to be available on either the decoder input or the encoder output port */
        vcos_semaphore_wait(&context.semaphore);

        /* Send data to decode to the input port of the video decoder */
        if ((buffer = mmal_queue_get(pool_in->queue)) != NULL)
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
        while ((buffer = mmal_queue_get(context.queue)) != NULL)
        {
            /* We have a frame, do something with it
                * Once we're done with it, we release it. It will automatically go back
                * to its original pool so it can be reused for a new video frame.
                */
            eos_received = buffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS;

            if (buffer->cmd)
            {
                fprintf(stderr, "received event %4.4s\n", (char *)&buffer->cmd);
                if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED)
                {
                    MMAL_EVENT_FORMAT_CHANGED_T *event = mmal_event_format_changed_get(buffer);
                    if (event)
                    {
                        fprintf(stderr, "----------Port format changed----------\n");
                        log_video_format(encoder->output[0]->format);
                        fprintf(stderr, "-----------------to---------------------\n");
                        log_video_format(event->format);
                        fprintf(stderr, " buffers num (opt %i, min %i), size (opt %i, min: %i)\n",
                                event->buffer_num_recommended, event->buffer_num_min,
                                event->buffer_size_recommended, event->buffer_size_min);
                        fprintf(stderr, "----------------------------------------\n");
                    }
                }

            }
            else
            {
                DEST_WRITE_DATA_INTO_FILE(buffer->data, buffer->length);
                fprintf(stderr, "encoded frame %u (flags %x, length %u)\n",framenr++, buffer->flags, buffer->length);
            }
            mmal_buffer_header_release(buffer);
        }

        /* Send empty buffers to the output port of the encoder */
        while ((buffer = mmal_queue_get(pool_out->queue)) != NULL)
            {
               status = mmal_port_send_buffer(encoder->output[0], buffer);
               CHECK_STATUS(status, "failed to send buffer");
            }


    }

    /* Stop decoding */
    fprintf(stderr, "stop decoding\n");

    /* Stop everything. Not strictly necessary since mmal_component_destroy()
       * will do that anyway */
    mmal_port_disable(decoder->input[0]);
    mmal_port_disable(decoder->control);
    mmal_port_disable(encoder->output[0]);

    /* Stop everything */
    fprintf(stderr, "stop");
    mmal_connection_disable(conn);

    SOURCE_CLOSE();
    DEST_CLOSE();

error:
    /* Cleanup everything */
    if (conn)
        mmal_connection_destroy(conn);
    if (decoder)
        mmal_component_release(decoder);
    if (encoder)
        mmal_component_release(encoder);

    return status == MMAL_SUCCESS ? 0 : -1;

}
