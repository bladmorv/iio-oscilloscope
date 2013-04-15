/**
 * Copyright (C) 2012-2013 Analog Devices, Inc.
 *
 * Licensed under the GPL-2.
 *
 **/
#include <stdio.h>

#include <gtk/gtk.h>
#include <gtkdatabox.h>
#include <gtkdatabox_grid.h>
#include <gtkdatabox_points.h>
#include <gtkdatabox_lines.h>
#include <math.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <malloc.h>
#include <dlfcn.h>

#include <fftw3.h>

#include "iio_widget.h"
#include "iio_utils.h"
#include "int_fft.h"
#include "config.h"
#include "osc_plugin.h"
#include "osc.h"

extern char dev_dir_name[512];

static gfloat *X = NULL;
static gfloat *fft_channel = NULL;

static gint capture_function = 0;
static int buffer_fd = -1;

static struct buffer data_buffer;
static unsigned int num_samples;

static struct iio_channel_info *channels;
static unsigned int num_active_channels;
static unsigned int num_channels;
static gfloat **channel_data;
static unsigned int current_sample;
static unsigned int bytes_per_sample;

static GtkWidget *databox;
static GtkWidget *sample_count_widget;
static GtkWidget *fft_size_widget;
static GtkWidget *fft_radio, *time_radio, *constellation_radio;
static GtkWidget *show_grid;
static GtkWidget *enable_auto_scale;
static GtkWidget *device_list_widget;
static GtkWidget *capture_button;

GtkWidget *capture_graph;

static GtkWidget *rx_lo_freq_label, *adc_freq_label;

static GtkDataboxGraph *fft_graph;
static GtkDataboxGraph *grid;

static GtkDataboxGraph **channel_graph;

static GtkListStore *channel_list_store;

static double adc_freq = 246760000.0;

static bool is_fft_mode;

static const char *current_device;

static GdkColor color_graph[] = {
	{
		.red = 0,
		.green = 60000,
		.blue = 0,
	},
	{
		.red = 60000,
		.green = 0,
		.blue = 0,
	},
	{
		.red = 0,
		.green = 0,
		.blue = 60000,
	},
	{
		.red = 0,
		.green = 60000,
		.blue = 60000,
	},
};

static GdkColor color_grid = {
	.red = 51000,
	.green = 51000,
	.blue = 0,
};

static GdkColor color_background = {
	.red = 0,
	.green = 0,
	.blue = 0,
};

/* Couple helper functions from fru parsing */
void printf_warn (const char * fmt, ...)
{
	return;
}


void printf_err (const char * fmt, ...)
{
	va_list ap;
	va_start(ap,fmt);
	vfprintf(stderr,fmt,ap);
	va_end(ap);
}


void * x_calloc (size_t nmemb, size_t size)
{
	unsigned int *ptr;

	ptr = calloc(nmemb, size);
	if (ptr == NULL)
		printf_err("memory error - calloc returned zero\n");
	return (void *)ptr;
}


struct buffer {
	void *data;
	unsigned int available;
	unsigned int size;
};

static bool is_oneshot_mode(void)
{
	if (strcmp(current_device, "cf-ad9643-core-lpc") == 0)
		return true;

	if (strncmp(current_device, "cf-ad9250", 9) == 0)
		return true;

	return false;
}

static int buffer_open(unsigned int length)
{
	int ret;
	int fd;

	if (!current_device)
		return -ENODEV;

	set_dev_paths(current_device);

	fd = iio_buffer_open();
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "Failed to open buffer: %d\n", ret);
		return ret;
	}

	/* Setup ring buffer parameters */
	ret = write_devattr_int("buffer/length", length);
	if (ret < 0) {
		fprintf(stderr, "Failed to set buffer length: %d\n", ret);
		goto err_close;
	}

	/* Enable the buffer */
	ret = write_devattr_int("buffer/enable", 1);
	if (ret < 0) {
		fprintf(stderr, "Failed to enable buffer: %d\n", ret);
		goto err_close;
	}

	return fd;

err_close:
	close(fd);
	return ret;
}

static void buffer_close(unsigned int fd)
{
	int ret;

	if (!current_device)
		return;

	set_dev_paths(current_device);

	/* Enable the buffer */
	ret = write_devattr_int("buffer/enable", 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to disable buffer: %d\n", ret);
	}

	close(fd);
}

#if DEBUG

static int sample_iio_data_continuous(int buffer_fd, struct buffer *buf)
{
	static int offset;
	int i;

	for (i = 0; i < num_samples; i++) {
		((int16_t *)(buf->data))[i*2] = 4096.0f * cos((i + offset) * G_PI / 100) + (rand() % 500 - 250);
		((int16_t *)(buf->data))[i*2+1] = 4096.0f * sin((i + offset) * G_PI / 100) + (rand() % 1000 - 500);
	}

	buf->available = 10;
	offset += 10;

	return 0;
}

static int sample_iio_data_oneshot(struct buffer *buf)
{
	return sample_iio_data(0, buf);
}

#else

static int sample_iio_data_continuous(int buffer_fd, struct buffer *buf)
{
	int ret;

	ret = read(buffer_fd, buf->data + buf->available,
			buf->size - buf->available);
	if (ret == 0)
		return -1;
	if (ret < 0)
		return ret;

	buf->available += ret;

	return 0;
}

static int sample_iio_data_oneshot(struct buffer *buf)
{
	int fd, ret;

	fd = buffer_open(buf->size);
	if (fd < 0)
		return fd;

	ret = sample_iio_data_continuous(fd, buf);

	buffer_close(fd);

	return ret;
}

#endif

static int sample_iio_data(struct buffer *buf)
{
	if (is_oneshot_mode())
		return sample_iio_data_oneshot(buf);
	else
		return sample_iio_data_continuous(buffer_fd, buf);
}

static int frame_counter;

static void fps_counter(void)
{
	static time_t last_update;
	time_t t;

	frame_counter++;
	t = time(NULL);
	if (t - last_update >= 10) {
		printf("FPS: %d\n", frame_counter / 10);
		frame_counter = 0;
		last_update = t;
	}
}

static void rescale_databox(GtkDatabox *box, gfloat border)
{
	bool fixed_aspect = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON (constellation_radio));

	if (fixed_aspect) {
		gfloat min_x;
		gfloat max_x;
		gfloat min_y;
		gfloat max_y;
		gfloat width;

		gint extrema_success = gtk_databox_calculate_extrema(box,
				&min_x, &max_x, &min_y, &max_y);
		if (extrema_success)
			return;
		if (min_x > min_y)
			min_x = min_y;
		if (max_x < max_y)
			max_x = max_y;

		width = max_x - min_x;
		if (width == 0)
			width = max_x;

		min_x -= border * width;
		max_x += border * width;

		gtk_databox_set_total_limits(box, min_x, max_x, max_x, min_x);

	} else {
		gtk_databox_auto_rescale(box, border);
	}
}

static void auto_scale_databox(GtkDatabox *box)
{
	if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enable_auto_scale)))
		return;

	/* Auto scale every 10 seconds */
	if (frame_counter == 0)
		rescale_databox(box, 0.05);
}

static int sign_extend(unsigned int val, unsigned int bits)
{
	bits -= 1;
	return ((int)(val << (31 - bits))) >> bits;
}

static void demux_data_stream(void *data_in, gfloat **data_out,
	unsigned int num_samples, unsigned int offset, unsigned int data_out_size,
	struct iio_channel_info *channels, unsigned int num_channels)
{
	unsigned int i, j, n;
	unsigned int val;
	unsigned int k;

	for (i = 0; i < num_samples; i++) {
		n = (offset + i) % data_out_size;
		k = 0;
		for (j = 0; j < num_channels; j++) {
			if (!channels[j].enabled)
				continue;
			switch (channels[j].bytes) {
			case 1:
				val = *(uint8_t *)data_in;
				break;
			case 2:
				switch (channels[j].endianness) {
				case IIO_BE:
					val = be16toh(*(uint16_t *)data_in);
					break;
				case IIO_LE:
					val = le16toh(*(uint16_t *)data_in);
					break;
				default:
					val = 0;
					break;
				}
				break;
			case 4:
				switch (channels[j].endianness) {
				case IIO_BE:
					val = be32toh(*(uint32_t *)data_in);
					break;
				case IIO_LE:
					val = le32toh(*(uint32_t *)data_in);
					break;
				default:
					val = 0;
					break;
				}
				break;
			default:
				continue;
			}
			data_in += channels[j].bytes;
			val >>= channels[j].shift;
			val &= channels[j].mask;
			if (channels[j].is_signed)
				data_out[k][n] = sign_extend(val, channels[j].bits_used);
			else
				data_out[k][n] = val;
			k++;
		}
	}

}

static void abort_sampling(void)
{
	if (buffer_fd >= 0) {
		buffer_close(buffer_fd);
		buffer_fd = -1;
	}
	gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(capture_button),
			FALSE);
}

static gboolean time_capture_func(GtkDatabox *box)
{
	unsigned int n;
	int ret;

	if (!GTK_IS_DATABOX(box))
		return FALSE;

	ret = sample_iio_data(&data_buffer);
	if (ret < 0) {
		abort_sampling();
		fprintf(stderr, "Failed to capture samples: %d\n", ret);
		return FALSE;
	}

	n = data_buffer.available / bytes_per_sample;

	demux_data_stream(data_buffer.data, channel_data, n, current_sample,
			num_samples, channels, num_channels);
	current_sample = (current_sample + n) % num_samples;
	data_buffer.available -= n * bytes_per_sample;
	if (data_buffer.available != 0) {
		memmove(data_buffer.data, data_buffer.data +  n * bytes_per_sample,
			data_buffer.available);
	}
/*
	for (j = 1; j < num_samples; j++) {
		if (data[j * 2 - 2] < trigger && data[j * 2] >= trigger)
			break;
	}
*/
	auto_scale_databox(box);

	gtk_widget_queue_draw(GTK_WIDGET(box));
	usleep(50000);

	fps_counter();

	return TRUE;
}

static void add_grid(void)
{
	grid = gtk_databox_grid_new(15, 15, &color_grid, 1);
	gtk_databox_graph_add(GTK_DATABOX(databox), grid);
	gtk_databox_graph_set_hide(grid, !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_grid)));
}

#if NO_FFTW

static void do_fft()
{
	unsigned int fft_size = num_samples;
	short *real, *imag, *amp, *fft_buf;
	unsigned int cnt, i;

	fft_buf = malloc((fft_size * 2 + fft_size / 2) * sizeof(short));
	if (fft_buf == NULL){
		fprintf(stderr, "malloc failed (%d)\n", __LINE__);
		return;
	}

	real = fft_buf;
	imag = real + fft_size;
	amp = imag+ fft_size;

	cnt = 0;
	for (i = 0; i < fft_size * 2; i += 2) {
		real[cnt] = ((int16_t *)(buf->data))[i];
		imag[cnt] = 0;
		cnt++;
	}

	window(real, fft_size);

	fix_fft(real, imag, (int)log2f(fft_size), 0);
	fix_loud(amp, real, imag, fft_size / 2, 2); /* scale 14->16 bit */

	for (i = 0; i < fft_size / 2; ++i)
		fft_channel[i] = amp[i];

	free(fft_buf);
}

#else

static double win_hanning(int j, int n)
{
    double a = 2.0*M_PI/(n-1), w;

    w = 0.5 * (1.0 - cos(a*j));

    return (w);
}

static void do_fft(struct buffer *buf)
{
	unsigned int fft_size = num_samples;
	int i;
	int cnt;
	static double *in;
	static double *win;
	static fftw_complex *out;
	static fftw_plan plan_forward;
	static int cached_fft_size = -1;

	if ((cached_fft_size == -1) || (cached_fft_size != fft_size)) {

		if (cached_fft_size != -1) {
			fftw_destroy_plan(plan_forward);
			fftw_free(in);
			fftw_free(win);
			fftw_free(out);
		}

		in = fftw_malloc(sizeof(double) * fft_size);
		win = fftw_malloc(sizeof(double) * fft_size);
		out = fftw_malloc(sizeof(fftw_complex) * ((fft_size / 2) + 1));
		plan_forward = fftw_plan_dft_r2c_1d(fft_size, in, out, FFTW_ESTIMATE);

		for (i = 0; i < fft_size; i ++)
			win[i] = win_hanning(i, fft_size);

		cached_fft_size = fft_size;
	}

	for (cnt = 0, i = 0; i < fft_size; i++) {
		in[cnt] = ((int16_t *)(buf->data))[i] * win[cnt];
		cnt++;
	}

	fftw_execute(plan_forward);

	for (i = 0; i < fft_size / 2; ++i)
		fft_channel[i] = 10 * log10((out[i][0] * out[i][0] + out[i][1] * out[i][1]) / (fft_size * fft_size)) - 50.0f;
}

#endif

static gboolean fft_capture_func(GtkDatabox *box)
{
	int ret;

	ret = sample_iio_data(&data_buffer);
	if (ret < 0) {
		abort_sampling();
		fprintf(stderr, "Failed to capture samples: %d\n", ret);
		return FALSE;
	}
	if (data_buffer.available == data_buffer.size) {
		do_fft(&data_buffer);
		data_buffer.available = 0;
		auto_scale_databox(box);
		gtk_widget_queue_draw(GTK_WIDGET(box));
	}

	usleep(50000);

	fps_counter();

	return TRUE;
}

static int fft_capture_setup(void)
{
	int i;

	if (num_active_channels != 1)
		return -EINVAL;

	num_samples = atoi(gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(fft_size_widget)));
	data_buffer.size = num_samples * bytes_per_sample;

	data_buffer.data = g_renew(int8_t, data_buffer.data, data_buffer.size);
	X = g_renew(gfloat, X, num_samples / 2);
	fft_channel = g_renew(gfloat, fft_channel, num_samples / 2);

	for (i = 0; i < num_samples / 2; i++)
	{
		X[i] = i * adc_freq / num_samples;
		fft_channel[i] = 0.0f;
	}
	is_fft_mode = true;

	fft_graph = gtk_databox_lines_new(num_samples / 2, X, fft_channel, &color_graph[0], 1);
	gtk_databox_graph_add(GTK_DATABOX(databox), fft_graph);

	gtk_databox_set_total_limits(GTK_DATABOX(databox), -5.0, adc_freq / 2.0 + 5.0, 0.0, -75.0);

	return 0;
}

static void fft_capture_start(void)
{
	capture_function = g_idle_add((GSourceFunc) fft_capture_func, databox);
}

static int time_capture_setup(void)
{
	gboolean is_constellation;
	unsigned int i, j;

	is_constellation = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON (constellation_radio));

	if (is_constellation && num_active_channels != 2)
		return -EINVAL;

	gtk_databox_graph_remove_all(GTK_DATABOX(databox));

	num_samples = gtk_spin_button_get_value(GTK_SPIN_BUTTON(sample_count_widget));
	data_buffer.size = num_samples * bytes_per_sample;

	data_buffer.data = g_renew(int8_t, data_buffer.data, data_buffer.size);
	X = g_renew(gfloat, X, num_samples);

	for (i = 0; i < num_samples; i++)
		X[i] = i;

	is_fft_mode = false;

	channel_data = g_renew(gfloat *, channel_data, num_active_channels);
	channel_graph = g_renew(GtkDataboxGraph *, channel_graph, num_active_channels);
	for (i = 0; i < num_active_channels; i++) {
		channel_data[i] = g_new(gfloat, num_samples);
		for (j = 0; j < num_samples; j++)
			channel_data[i][j] = 0.0f;
	}

	if (is_constellation) {
		fft_graph = gtk_databox_lines_new(num_samples, channel_data[0],
					channel_data[1], &color_graph[0], 1);
		gtk_databox_graph_add(GTK_DATABOX (databox), fft_graph);
	} else {
		j = 0;
		for (i = 0; i < num_channels; i++) {
			if (!channels[i].enabled)
				continue;

			channel_graph[j] = gtk_databox_lines_new(num_samples, X,
				channel_data[j], &color_graph[i], 1);
			gtk_databox_graph_add(GTK_DATABOX(databox), channel_graph[j]);
			j++;
		}
	}

	if (is_constellation)
		gtk_databox_set_total_limits(GTK_DATABOX(databox), -8500.0, 8500.0, 8500.0, -8500.0);
	else
		gtk_databox_set_total_limits(GTK_DATABOX(databox), 0.0, num_samples, 8500.0, -8500.0);

	return 0;
}

static void time_capture_start()
{
	capture_function = g_idle_add((GSourceFunc) time_capture_func, databox);
}

static void capture_button_clicked(GtkToggleToolButton *btn, gpointer data)
{
	unsigned int i;
	int ret;

	if (gtk_toggle_tool_button_get_active(btn)) {
		gtk_databox_graph_remove_all(GTK_DATABOX(databox));

		data_buffer.available = 0;
		current_sample = 0;
		num_active_channels = 0;
		bytes_per_sample = 0;
		for (i = 0; i < num_channels; i++) {
			if (channels[i].enabled) {
				bytes_per_sample += channels[i].bytes;
				num_active_channels++;
			}
		}

		if (num_active_channels == 0 || !current_device) {
			gtk_toggle_tool_button_set_active(btn, FALSE);
			return;
		}

		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fft_radio)))
			ret = fft_capture_setup();
		else
			ret = time_capture_setup();

		if (ret) {
			gtk_toggle_tool_button_set_active(btn, FALSE);
			return;
		}

		if (!is_oneshot_mode()) {
			buffer_fd = buffer_open(num_samples);
			if (buffer_fd < 0) {
				gtk_toggle_tool_button_set_active(btn, FALSE);
				return;
			}
		}

		add_grid();
		gtk_widget_queue_draw(GTK_WIDGET(databox));
		frame_counter = 0;

		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fft_radio)))
			fft_capture_start();
		else
			time_capture_start();

	} else {
		if (capture_function > 0) {
			g_source_remove(capture_function);
			capture_function = 0;
		}
		if (buffer_fd >= 0) {
			buffer_close(buffer_fd);
			buffer_fd = -1;
		}
	}
}

static void show_grid_toggled(GtkToggleButton *btn, gpointer data)
{
	if (grid) {
		gtk_databox_graph_set_hide(grid, !gtk_toggle_button_get_active(btn));
		gtk_widget_queue_draw(GTK_WIDGET (data));
	}
}

static double read_sampling_frequency(void)
{
	double freq = 1.0;
	int ret;

	set_dev_paths(current_device);

	if (iio_devattr_exists(current_device, "in_voltage_sampling_frequency")) {
		read_devattr_double("in_voltage_sampling_frequency", &freq);
	} else if (iio_devattr_exists(current_device, "sampling_frequency")) {
		read_devattr_double("sampling_frequency", &freq);
	} else {
		char *trigger;

		ret = read_devattr("trigger/current_trigger", &trigger);
		if (ret >= 0) {
			if (*trigger != '\0') {
				set_dev_paths(trigger);
				if (iio_devattr_exists(trigger, "frequency"))
					read_devattr_double("frequency", &freq);
			}
		}

		free(trigger);
	}

	return freq;
}

void rx_update_labels(void)
{
	double freq = 2400000000.0;
	char buf[20];
	int i;

	adc_freq = read_sampling_frequency();
	if (adc_freq >= 1000000)
		snprintf(buf, sizeof(buf), "%.4f MHz", adc_freq / 1000000);
	else if(adc_freq >= 1000)
		snprintf(buf, sizeof(buf), "%.3f kHz", adc_freq / 1000);
	else
		snprintf(buf, sizeof(buf), "%.0f Hz", adc_freq);

	gtk_label_set_text(GTK_LABEL(adc_freq_label), buf);

	set_dev_paths("adf4351-rx-lpc");
	read_devattr_double("out_altvoltage0_frequency", &freq);
	freq /= 1000000.0;
	snprintf(buf, sizeof(buf), "%.4f Mhz", freq);
	gtk_label_set_text(GTK_LABEL(rx_lo_freq_label), buf);

	if (is_fft_mode) {
		/*
		 * In FFT mode we need to scale the X-axis according to the selected
		 * sampling frequency.
		 */
		for (i = 0; i < num_samples / 2; i++)
			X[i] = i * adc_freq / num_samples;
		gtk_databox_set_total_limits(GTK_DATABOX(databox), 0.0, adc_freq / 2.0, 0.0, -75.0);
	}
}

static void zoom_fit(GtkButton *btn, gpointer data)
{
	rescale_databox(GTK_DATABOX(data), 0.05);
}

static void zoom_in(GtkButton *btn, gpointer data)
{
	bool fixed_aspect = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON (constellation_radio));
	gfloat left, right, top, bottom;
	gfloat width, height;

	gtk_databox_get_visible_limits(GTK_DATABOX(data), &left, &right, &top, &bottom);
	width = right - left;
	height = bottom - top;
	left += width * 0.25;
	right -= width * 0.25;
	top += height * 0.25;
	bottom -= height * 0.25;

	if (fixed_aspect) {
		gfloat diff;
		width *= 0.5;
		height *= -0.5;
		if (height > width) {
			diff = width - height;
			left -= diff * 0.5;
			right += diff * 0.5;
		} else {
			diff = height - width;
			bottom += diff * 0.5;
			top -= diff * 0.5;
		}
	}

	gtk_databox_set_visible_limits(GTK_DATABOX(data), left, right, top, bottom);
}

static void zoom_out(GtkButton *btn, gpointer data)
{
	bool fixed_aspect = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON (constellation_radio));
	gfloat left, right, top, bottom;
	gfloat t_left, t_right, t_top, t_bottom;
	gfloat width, height;

	gtk_databox_get_visible_limits(GTK_DATABOX(data), &left, &right, &top, &bottom);
	width = right - left;
	height = bottom - top;
	left -= width * 0.25;
	right += width * 0.25;
	top -= height * 0.25;
	bottom += height * 0.25;

	gtk_databox_get_total_limits(GTK_DATABOX(data), &t_left, &t_right, &t_top, &t_bottom);
	if (left < right) {
		if (left < t_left)
			left = t_left;
		if (right > t_right)
			right = t_right;
	} else {
		if (left > t_left)
			left = t_left;
		if (right < t_right)
			right = t_right;
	}

	if (top < bottom) {
		if (top < t_top)
			top = t_top;
		if (bottom > t_bottom)
			bottom = t_bottom;
	} else {
		if (top > t_top)
			top = t_top;
		if (bottom < t_bottom)
			bottom = t_bottom;
	}

	if (fixed_aspect) {
		gfloat diff;
		width = right - left;
		height = top - bottom;
		if (height < width) {
			diff = width - height;
			bottom -= diff * 0.5;
			top += diff * 0.5;
			if (top < t_top) {
				bottom += t_top - top;
				top = t_top;
			}
			if (bottom > t_bottom) {
				top -= bottom - t_bottom;
				bottom = t_bottom;
			}
		} else {
			diff = height - width;
			left -= diff * 0.5;
			right += diff * 0.5;
			if (left < t_left) {
				right += t_left - left;
				left = t_left;
			}
			if (right > t_right) {
				left -= right - t_right;
				right = t_right;
			}
		}
		width = right - left;
		height = top - bottom;
	}

	gtk_databox_set_visible_limits(GTK_DATABOX(data), left, right, top, bottom);
}

static bool force_plugin(const char *name)
{
	const char *force_plugin = getenv("OSC_FORCE_PLUGIN");
	const char *pos;

	if (!force_plugin)
		return false;

	if (strcmp(force_plugin, "all") == 0)
		return true;

	pos = strcasestr(force_plugin, name);
	if (pos) {
		switch (*(pos + strlen(name))) {
		case ' ':
		case '\0':
			return true;
		default:
			break;
		}
	}

	return false;
}

static void load_plugin(const char *name, GtkWidget *notebook)
{
	const struct osc_plugin *plugin;
	void *lib;

	lib = dlopen(name, RTLD_LOCAL | RTLD_LAZY);
	if (!lib) {
		fprintf(stderr, "Failed to load plugin \"%s\": %s\n", name, dlerror());
		return;
	}

	plugin = dlsym(lib, "plugin");
	if (!plugin) {
		fprintf(stderr, "Failed to load plugin \"%s\": Could not find plugin\n",
				name);
		return;
	}

	printf("Found plugin: %s\n", plugin->name);

	if (!plugin->identify() && !force_plugin(plugin->name))
		return;

	plugin->init(notebook);

	printf("Loaded plugin: %s\n", plugin->name);
}

static bool str_endswith(const char *str, const char *needle)
{
	const char *pos;
	pos = strstr(str, needle);
	if (pos == NULL)
		return false;
	return *(pos + strlen(needle)) == '\0';
}

static void load_plugins(GtkWidget *notebook)
{
	struct dirent *ent;
	char buf[512];
	DIR *d;

	/* Check the local plugins folder first */
	d = opendir("plugins");
	if (!d)
		d = opendir(OSC_PLUGIN_PATH);

	while ((ent = readdir(d))) {
		if (ent->d_type != DT_REG)
			continue;
		if (!str_endswith(ent->d_name, ".so"))
			continue;
		snprintf(buf, sizeof(buf), "plugins/%s", ent->d_name);
		load_plugin(buf, notebook);
	}
}

static bool is_input_device(const char *device)
{
	struct iio_channel_info *channels = NULL;
	unsigned int num_channels;
	bool is_input = false;
	int ret;
	int i;

	set_dev_paths(device);

	ret = build_channel_array(dev_dir_name, &channels, &num_channels);
	if (ret)
		return false;

	for (i = 0; i < num_channels; i++) {
		if (strncmp("in", channels[i].name, 2) == 0) {
			is_input = true;
			break;
		}
	}

	free_channel_array(channels, num_channels);

	return is_input;
}

static void device_list_cb(GtkWidget *widget, gpointer data)
{
	GtkTreeIter iter;
	int ret;
	int i;

	gtk_list_store_clear(channel_list_store);
	if (num_channels)
		free_channel_array(channels, num_channels);

	current_device = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(device_list_widget));

	if (!current_device)
		return;

	set_dev_paths(current_device);

	ret = build_channel_array(dev_dir_name, &channels, &num_channels);
	if (ret)
		return;

	for (i = 0; i < num_channels; i++) {
		if (strncmp("in", channels[i].name, 2) == 0 &&
			strcmp("in_timestamp", channels[i].name) != 0)
		{
			gtk_list_store_append(channel_list_store, &iter);
			gtk_list_store_set(channel_list_store, &iter, 0, channels[i].name,
				1, channels[i].enabled, 2, &channels[i], -1);
		}
	}

}

static void init_device_list(void)
{
	char *devices = NULL, *device;
	unsigned int num;

	g_signal_connect(device_list_widget, "changed",
			G_CALLBACK(device_list_cb), NULL);

	num = find_iio_names(&devices, NULL, NULL);
	if (devices == NULL)
		return;

	device = devices;
	for (; num > 0; num--) {
		if (is_input_device(device)) {
			gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(device_list_widget),
					device);
		}
		device += strlen(device) + 1;
	}
	free(devices);

	gtk_combo_box_set_active(GTK_COMBO_BOX(device_list_widget), 0);

	device_list_cb(device_list_widget, NULL);
}

void channel_toggled(GtkCellRendererToggle* renderer, gchar* pathStr, gpointer data)
{
	GtkTreePath* path = gtk_tree_path_new_from_string(pathStr);
	struct iio_channel_info *channel;
	GtkTreeIter iter;
	unsigned int enabled;
	char buf[512];
	FILE *f;

	set_dev_paths(current_device);

	gtk_tree_model_get_iter(GTK_TREE_MODEL (data), &iter, path);
	gtk_tree_model_get(GTK_TREE_MODEL (data), &iter, 1, &enabled, 2, &channel, -1);
	enabled = !enabled;

	snprintf(buf, sizeof(buf), "%s/scan_elements/%s_en", dev_dir_name, channel->name);
	f = fopen(buf, "w");
	fprintf(f, "%u\n", enabled);
	fclose(f);
	f = fopen(buf, "r");
	fscanf(f, "%u", &enabled);
	fclose(f);

	channel->enabled = enabled;
	gtk_list_store_set(GTK_LIST_STORE (data), &iter, 1, enabled, -1);
}

void application_quit (void)
{
	if (capture_function > 0) {
		g_source_remove(capture_function);
		capture_function = 0;
	}
	if (buffer_fd >= 0) {
		buffer_close(buffer_fd);
		buffer_fd = -1;
	}

	gtk_main_quit();
}

void sigterm (int signum)
{
	application_quit();
}

static void init_application (void)
{
	GtkWidget *window;
	GtkWidget *table;
	GtkWidget *tmp;
	GtkWidget *notebook;
	GtkBuilder *builder;

	builder = gtk_builder_new();

	if (!gtk_builder_add_from_file(builder, "./osc.glade", NULL))
		gtk_builder_add_from_file(builder, OSC_GLADE_FILE_PATH "osc.glade", NULL);
	else {
		GtkImage *logo;
		GtkAboutDialog *about;
		GdkPixbuf *pixbuf;
		GError *err = NULL;

		/* We are running locally, so load the local files */
		logo = GTK_IMAGE(gtk_builder_get_object(builder, "ADI_logo"));
		g_object_set(logo, "file","./icons/ADIlogo.png", NULL);
		logo = GTK_IMAGE(gtk_builder_get_object(builder, "about_ADI_logo"));
		g_object_set(logo, "file","./icons/ADIlogo.png", NULL);
		logo = GTK_IMAGE(gtk_builder_get_object(builder, "about_IIO_logo"));
		g_object_set(logo, "file","./icons/IIOlogo.png", NULL);
		about = GTK_ABOUT_DIALOG(gtk_builder_get_object(builder, "About_dialog"));
		pixbuf = gdk_pixbuf_new_from_file("./icons/osc128.png", &err);
		if (pixbuf)
			g_object_set(about, "logo", pixbuf,  NULL);
	}

	window = GTK_WIDGET(gtk_builder_get_object(builder, "toplevel"));
	capture_graph = GTK_WIDGET(gtk_builder_get_object(builder, "display_capture"));
	sample_count_widget = GTK_WIDGET(gtk_builder_get_object(builder, "sample_count"));
	fft_size_widget = GTK_WIDGET(gtk_builder_get_object(builder, "fft_size"));
	fft_radio = GTK_WIDGET(gtk_builder_get_object(builder, "type_fft"));
	time_radio = GTK_WIDGET(gtk_builder_get_object(builder, "type"));
	constellation_radio = GTK_WIDGET(gtk_builder_get_object(builder, "type_constellation"));
	adc_freq_label = GTK_WIDGET(gtk_builder_get_object(builder, "adc_freq_label"));
	rx_lo_freq_label = GTK_WIDGET(gtk_builder_get_object(builder, "rx_lo_freq_label"));
	show_grid = GTK_WIDGET(gtk_builder_get_object(builder, "show_grid"));
	enable_auto_scale = GTK_WIDGET(gtk_builder_get_object(builder, "auto_scale"));
	notebook = GTK_WIDGET(gtk_builder_get_object(builder, "notebook"));
	device_list_widget = GTK_WIDGET(gtk_builder_get_object(builder, "input_device_list"));
	capture_button = GTK_WIDGET(gtk_builder_get_object(builder, "capture_button"));

	channel_list_store = GTK_LIST_STORE(gtk_builder_get_object(builder, "channel_list"));
	g_builder_connect_signal(builder, "channel_toggle", "toggled",
		G_CALLBACK(channel_toggled), channel_list_store);

	dialogs_init(builder);

	gtk_combo_box_set_active(GTK_COMBO_BOX(fft_size_widget), 0);

	/* Bind the plot mode radio buttons to the sensitivity of the sample count
	 * and FFT size widgets */
	tmp = GTK_WIDGET(gtk_builder_get_object(builder, "fft_size_label"));
	g_object_bind_property(fft_radio, "active", tmp, "sensitive", 0);
	g_object_bind_property(fft_radio, "active", fft_size_widget, "sensitive", 0);
	tmp = GTK_WIDGET(gtk_builder_get_object(builder, "sample_count_label"));
	g_object_bind_property(fft_radio, "active", tmp, "sensitive", G_BINDING_INVERT_BOOLEAN);
	g_object_bind_property(fft_radio, "active", sample_count_widget, "sensitive", G_BINDING_INVERT_BOOLEAN);

	num_samples = 1;
	X = g_renew(gfloat, X, num_samples);
	fft_channel = g_renew(gfloat, fft_channel, num_samples);

	/* Create a GtkDatabox widget along with scrollbars and rulers */
	gtk_databox_create_box_with_scrollbars_and_rulers(&databox, &table,
							  TRUE, TRUE, TRUE, TRUE);
	gtk_box_pack_start(GTK_BOX(capture_graph), table, TRUE, TRUE, 0);
	gtk_widget_modify_bg(databox, GTK_STATE_NORMAL, &color_background);

	add_grid();

	gtk_widget_set_size_request(table, 600, 600);

	g_builder_connect_signal(builder, "capture_button", "toggled",
		G_CALLBACK(capture_button_clicked), NULL);
	g_builder_connect_signal(builder, "zoom_in", "clicked",
		G_CALLBACK(zoom_in), databox);
	g_builder_connect_signal(builder, "zoom_out", "clicked",
		G_CALLBACK(zoom_out), databox);
	g_builder_connect_signal(builder, "zoom_fit", "clicked",
		G_CALLBACK(zoom_fit), databox);
	g_signal_connect(G_OBJECT(show_grid), "toggled",
		G_CALLBACK(show_grid_toggled), databox);

	g_signal_connect(G_OBJECT(window), "destroy",
			 G_CALLBACK(application_quit), NULL);

	g_builder_bind_property(builder, "capture_button", "active",
			"channel_list_view", "sensitive", G_BINDING_INVERT_BOOLEAN);
	g_builder_bind_property(builder, "capture_button", "active",
			"input_device_list", "sensitive", G_BINDING_INVERT_BOOLEAN);

	init_device_list();
	load_plugins(notebook);
	rx_update_labels();

	gtk_widget_show_all(window);
}

gint main(gint argc, char *argv[])
{
	gtk_init(&argc, &argv);
	signal(SIGTERM, sigterm);
	signal(SIGINT, sigterm);
	signal(SIGHUP, sigterm);
	init_application();
	gtk_main();

	return 0;
}
