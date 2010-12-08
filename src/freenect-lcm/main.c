#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <sys/time.h>

#include <glib.h>
#include <lcm/lcm.h>
#include <libfreenect.h>

#include <lcmtypes/kinect_depth_data_t.h>
#include <lcmtypes/kinect_image_data_t.h>

#include "timestamp.h"

#define dbg(...) fprintf(stderr, __VA_ARGS__)

typedef struct _state_t {
    GThread* freenect_thread;
    volatile int die;

    freenect_context *f_ctx;
    freenect_device *f_dev;
    int freenect_angle;
    int freenect_led;

    freenect_video_format requested_image_format;
    freenect_video_format current_image_format;

    freenect_depth_format requested_depth_format;
    freenect_depth_format current_depth_format;

    uint8_t* image_data;

    uint8_t* image_buf;
    int image_buf_size;

    uint8_t* depth_buf;
    int depth_buf_size;

    freenect_raw_tilt_state* tilt_state;
    double accel_mks[3];
    double tilt_radians;

    int8_t current_led;
    int8_t requested_led;

    char* image_channel;
    char* depth_channel;

    kinect_image_data_t image_msg;
    kinect_depth_data_t depth_msg;

    int have_img;
    int have_depth;

    timestamp_sync_state_t* clocksync;

    lcm_t* lcm;
} state_t;

#if 0

void keyPressed(unsigned char key, int x, int y)
{
	if (key == 27) {
		die = 1;
		pthread_join(freenect_thread, NULL);
		glutDestroyWindow(window);
		free(depth_data);
		free(depth_front);
		free(image_data);
		free(rgb_mid);
		free(rgb_front);
		pthread_exit(NULL);
	}
	if (key == 'w') {
		freenect_angle++;
		if (freenect_angle > 30) {
			freenect_angle = 30;
		}
	}
	if (key == 's') {
		freenect_angle = 0;
	}
	if (key == 'f') {
		if (requested_image_format == FREENECT_VIDEO_IR_8BIT)
			requested_image_format = FREENECT_VIDEO_RGB;
		else if (requested_image_format == FREENECT_VIDEO_RGB)
			requested_image_format = FREENECT_VIDEO_YUV_RGB;
		else
			requested_image_format = FREENECT_VIDEO_IR_8BIT;
	}
	if (key == 'x') {
		freenect_angle--;
		if (freenect_angle < -30) {
			freenect_angle = -30;
		}
	}
	if (key == '1') {
		freenect_set_led(f_dev,LED_GREEN);
	}
	if (key == '2') {
		freenect_set_led(f_dev,LED_RED);
	}
	if (key == '3') {
		freenect_set_led(f_dev,LED_YELLOW);
	}
	if (key == '4') {
		freenect_set_led(f_dev,LED_BLINK_YELLOW);
	}
	if (key == '5') {
		freenect_set_led(f_dev,LED_BLINK_GREEN);
	}
	if (key == '6') {
		freenect_set_led(f_dev,LED_BLINK_RED_YELLOW);
	}
	if (key == '0') {
		freenect_set_led(f_dev,LED_OFF);
	}
	freenect_set_tilt_degs(f_dev,freenect_angle);
}

uint16_t t_gamma[2048];
#endif

static void
populate_status_t(state_t* state, kinect_status_t* msg, int64_t timestamp)
{
    freenect_update_tilt_state(state->f_dev);
    state->tilt_state = freenect_get_tilt_state(state->f_dev);

    double dx,dy,dz;
    freenect_get_mks_accel(state->tilt_state, &dx, &dy, &dz);

    msg->timestamp = timestamp;
    msg->raw_accel[0] = state->tilt_state->accelerometer_x;
    msg->raw_accel[1] = state->tilt_state->accelerometer_y;
    msg->raw_accel[2] = state->tilt_state->accelerometer_z;
    msg->raw_tilt = state->tilt_state->tilt_angle;

    msg->accel[0] = dx;
    msg->accel[1] = dy;
    msg->accel[2] = dz;
    msg->tilt_radians = freenect_get_tilt_degs(state->tilt_state) * M_PI / 180;

    msg->led_status = state->current_led;

    switch(state->tilt_state->tilt_status) {
        case TILT_STATUS_STOPPED:
            msg->tilt_status = KINECT_STATUS_T_TILT_STATUS_STOPPED;
            break;
        case TILT_STATUS_LIMIT:
            msg->tilt_status = KINECT_STATUS_T_TILT_STATUS_LIMIT;
            break;
        case TILT_STATUS_MOVING:
            msg->tilt_status = KINECT_STATUS_T_TILT_STATUS_MOVING;
            break;
    }
    msg->tilt_status = state->tilt_state->tilt_status;
}

void depth_cb(freenect_device *dev, void *data, uint32_t timestamp)
{
    state_t* state = (state_t*) freenect_get_user(dev);

    int64_t host_utime = timestamp_now();
    state->depth_msg.timestamp = timestamp_sync(state->clocksync, timestamp, host_utime);

    switch(state->current_depth_format) {
        case FREENECT_DEPTH_11BIT:
            state->depth_msg.depth_data_nbytes = FREENECT_DEPTH_11BIT_SIZE;
            state->depth_msg.depth_data_format = KINECT_DEPTH_DATA_T_DEPTH_11BIT;
            break;
        case FREENECT_DEPTH_10BIT:
            state->depth_msg.depth_data_nbytes = FREENECT_DEPTH_10BIT_SIZE;
            state->depth_msg.depth_data_format = KINECT_DEPTH_DATA_T_DEPTH_10BIT;
            break;
        case FREENECT_DEPTH_11BIT_PACKED:
            state->depth_msg.depth_data_nbytes = FREENECT_DEPTH_11BIT_PACKED_SIZE;
            state->depth_msg.depth_data_format = KINECT_DEPTH_DATA_T_DEPTH_11BIT_PACKED;
            break;
        case FREENECT_DEPTH_10BIT_PACKED:
            state->depth_msg.depth_data_nbytes = FREENECT_DEPTH_10BIT_PACKED_SIZE;
            state->depth_msg.depth_data_format = KINECT_DEPTH_DATA_T_DEPTH_10BIT_PACKED;
            break;
        default:
            state->depth_msg.depth_data_nbytes = 0;
            state->depth_msg.depth_data_format = 0; // TODO spew warning
            break;
    }

    assert(state->depth_msg.depth_data_nbytes < state->depth_buf_size);
    memcpy(state->depth_buf, data, state->depth_msg.depth_data_nbytes);
    state->have_img++;
}

void image_cb(freenect_device *dev, void *data, uint32_t timestamp)
{
    state_t* state = (state_t*) freenect_get_user(dev);

    int64_t host_utime = timestamp_now();
    state->image_msg.timestamp = timestamp_sync(state->clocksync, timestamp, host_utime);

    switch(state->current_image_format) {
        case FREENECT_VIDEO_RGB:
            state->image_msg.image_data_nbytes =  FREENECT_VIDEO_RGB_SIZE;
            state->image_msg.image_data_format = KINECT_IMAGE_DATA_T_VIDEO_RGB;
            break;
        case FREENECT_VIDEO_BAYER:
            state->image_msg.image_data_nbytes =  FREENECT_VIDEO_BAYER_SIZE;
            state->image_msg.image_data_format = KINECT_IMAGE_DATA_T_VIDEO_BAYER;
            break;
        case FREENECT_VIDEO_YUV_RGB:
            state->image_msg.image_data_nbytes =  FREENECT_VIDEO_YUV_RGB_SIZE;
            state->image_msg.image_data_format = KINECT_IMAGE_DATA_T_VIDEO_YUV_RGB;
            break;
        case FREENECT_VIDEO_YUV_RAW:
            state->image_msg.image_data_nbytes =  FREENECT_VIDEO_YUV_RAW_SIZE;
            state->image_msg.image_data_format = KINECT_IMAGE_DATA_T_VIDEO_YUV_RAW;
            break;
        case FREENECT_VIDEO_IR_8BIT:
            state->image_msg.image_data_nbytes =  FREENECT_VIDEO_IR_8BIT_SIZE;
            state->image_msg.image_data_format = KINECT_IMAGE_DATA_T_VIDEO_IR_8BIT;
            break;
        case FREENECT_VIDEO_IR_10BIT:
            state->image_msg.image_data_nbytes =  FREENECT_VIDEO_IR_10BIT_SIZE;
            state->image_msg.image_data_format = KINECT_IMAGE_DATA_T_VIDEO_IR_10BIT;
            break;
        case FREENECT_VIDEO_IR_10BIT_PACKED:
            state->image_msg.image_data_nbytes =  FREENECT_VIDEO_IR_10BIT_PACKED_SIZE;
            state->image_msg.image_data_format = KINECT_IMAGE_DATA_T_VIDEO_IR_10BIT_PACKED;
            break;
        default:
            state->image_msg.image_data_nbytes = 0; // TODO spew warning
            state->image_msg.image_data_format = -1;
            break;
    }
    assert(state->image_msg.image_data_nbytes < state->image_buf_size);
    memcpy(state->image_buf, data, state->image_msg.image_data_nbytes);
    state->have_depth++;
}

static void *
freenect_threadfunc(void *user_data)
{
    printf("starting kinect thread...\n");
    state_t* state = (state_t*) user_data;

	freenect_set_tilt_degs(state->f_dev, state->freenect_angle);
	freenect_set_led(state->f_dev, state->current_led);
	freenect_set_depth_callback(state->f_dev, depth_cb);
	freenect_set_video_callback(state->f_dev, image_cb);
	freenect_set_video_format(state->f_dev, state->current_image_format);
	freenect_set_depth_format(state->f_dev, state->current_depth_format);
	freenect_set_video_buffer(state->f_dev, state->image_data);

	freenect_start_depth(state->f_dev);
	freenect_start_video(state->f_dev);

    state->have_img = 0;
    state->have_depth = 0;
	while (!state->die && freenect_process_events(state->f_ctx) >= 0) {
        if(state->have_img) {
            populate_status_t(state, &state->image_msg.status, state->image_msg.timestamp);
            kinect_image_data_t_publish(state->lcm, state->image_channel, &state->image_msg);

            dbg("published image data\n");
            state->have_img = 0;
        } else if(state->have_depth) {
            populate_status_t(state, &state->depth_msg.status, state->depth_msg.timestamp);
            kinect_depth_data_t_publish(state->lcm, state->depth_channel, &state->depth_msg);

            dbg("published depth data\n");
            state->have_depth = 0;
        }

		if (state->requested_image_format != state->current_image_format) {
			freenect_stop_video(state->f_dev);
			freenect_set_video_format(state->f_dev, state->requested_image_format);
			freenect_start_video(state->f_dev);
			state->current_image_format = state->requested_image_format;
		}
	}

	printf("\nshutting down streams...\n");

	freenect_stop_depth(state->f_dev);
	freenect_stop_video(state->f_dev);

	freenect_close_device(state->f_dev);
	freenect_shutdown(state->f_ctx);

	printf("-- done!\n");
	return NULL;
}

int main(int argc, char **argv)
{
    state_t *state = (state_t*)calloc(1, sizeof(state_t));
    state->freenect_thread = NULL;
    state->die = 0;
    state->f_ctx = NULL;
    state->f_dev = NULL;
    state->freenect_angle = 0;
    state->freenect_led = 0;

    state->requested_image_format = FREENECT_VIDEO_RGB;
    state->requested_depth_format = FREENECT_DEPTH_11BIT;
    state->requested_led = LED_RED;
    state->current_image_format = state->requested_image_format;
    state->current_depth_format = state->requested_depth_format;
    state->current_led = state->requested_led;

    // 
	int user_device_number = 0;
	if (argc > 1)
		user_device_number = atoi(argv[1]);

    // allocate image and depth buffers
	state->image_data   = (uint8_t*) malloc(640*480*4);

    state->image_buf_size = 640 * 480 * 4;
    state->image_buf = (uint8_t*) malloc(state->image_buf_size);
    state->image_msg.image_data = state->image_buf;
    state->image_msg.width = FREENECT_FRAME_W;
    state->image_msg.height = FREENECT_FRAME_H;

    state->depth_buf_size = 640 * 480 * sizeof(float);
    state->depth_buf = (uint8_t*) malloc(state->depth_buf_size);
    state->depth_msg.depth_data = state->depth_buf;
    state->depth_msg.width = FREENECT_FRAME_W;
    state->depth_msg.height = FREENECT_FRAME_H;

    state->have_depth = 0;
    state->have_img = 0;

    // initialize LCM
    state->image_channel = g_strdup("KINECT_IMAGE");
    state->depth_channel = g_strdup("KINECT_DEPTH");

    state->lcm = lcm_create(NULL);
    if(!state->lcm) {
        fprintf(stderr, "Unable to initialize LCM\n");
        return 1;
    }

    // initialize the kinect device
	if(freenect_init(&state->f_ctx, NULL) < 0) {
		printf("freenect_init() failed\n");
		return 1;
	}

	freenect_set_log_level(state->f_ctx, FREENECT_LOG_INFO);

	int num_devices = freenect_num_devices(state->f_ctx);
	printf("Number of devices found: %d\n", num_devices);

	if (num_devices < 1)
		return 1;

	if (freenect_open_device(state->f_ctx, &state->f_dev, user_device_number) < 0) {
		printf("Could not open device\n");
		return 1;
	}

    freenect_set_user(state->f_dev, state);

    state->clocksync = timestamp_sync_init(1000000, 0xFFFFFFFF, 1.05);

    g_thread_init(NULL);

    GError *thread_err = NULL;
    state->freenect_thread = g_thread_create(freenect_threadfunc, state, TRUE, &thread_err);
    if(thread_err) {
        fprintf(stderr, "Error creating thread: %s\n", thread_err->message);
        return 1;
    }

    // TODO subscribe to kinect command messages...

    while(!state->die) {
        lcm_handle(state->lcm);
    }

    timestamp_sync_free(state->clocksync);
    free(state->image_channel);
    free(state->depth_channel);
    free(state);

	return 0;
}
