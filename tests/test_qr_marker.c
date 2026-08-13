#include "quirc.h"
#include "qr_marker_render.h"
#include "waybeam_qr_format.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUICK_IMAGE_W 640
#define QUICK_IMAGE_H 480
#define EXTENDED_IMAGE_W 1280
#define EXTENDED_IMAGE_H 720
static const char expected_payload[] = QR_EXPECTED_PAYLOAD;

struct benchmark_case {
	const char *name;
	struct quirc_point corners[4];
};

static const struct benchmark_case benchmark_cases[] = {
	{
		"front",
		{{150, 70}, {480, 70}, {480, 400}, {150, 400}}
	},
	{
		"rotate-90",
		{{500, 60}, {500, 420}, {140, 420}, {140, 60}}
	},
	{
		"diamond",
		{{300, 25}, {575, 205}, {340, 455}, {65, 235}}
	},
	{
		"perspective",
		{{90, 65}, {570, 135}, {465, 430}, {165, 360}}
	},
	{
		"perspective-severe",
		{{75, 90}, {580, 175}, {420, 345}, {175, 395}}
	},
	{
		"mirror",
		{{500, 70}, {120, 70}, {120, 430}, {500, 430}}
	},
	{
		"small-perspective",
		{{235, 150}, {420, 170}, {385, 315}, {255, 300}}
	}
};

static void perspective_setup(double *c, const struct quirc_point *rect,
			      double w, double h)
{
	double x0 = rect[0].x;
	double y0 = rect[0].y;
	double x1 = rect[1].x;
	double y1 = rect[1].y;
	double x2 = rect[2].x;
	double y2 = rect[2].y;
	double x3 = rect[3].x;
	double y3 = rect[3].y;
	double wden = 1.0 /
		(w * (x2 * y3 - x3 * y2 + (x3 - x2) * y1 +
		      x1 * (y2 - y3)));
	double hden = 1.0 /
		(h * (x2 * y3 + x1 * (y2 - y3) - x3 * y2 +
		      (x3 - x2) * y1));

	c[0] = (x1 * (x2 * y3 - x3 * y2) +
		x0 * (-x2 * y3 + x3 * y2 + (x2 - x3) * y1) +
		x1 * (x3 - x2) * y0) * wden;
	c[1] = -(x0 * (x2 * y3 + x1 * (y2 - y3) - x2 * y1) -
		 x1 * x3 * y2 + x2 * x3 * y1 +
		 (x1 * x3 - x2 * x3) * y0) * hden;
	c[2] = x0;
	c[3] = (y0 * (x1 * (y3 - y2) - x2 * y3 + x3 * y2) +
		y1 * (x2 * y3 - x3 * y2) + x0 * y1 * (y2 - y3)) * wden;
	c[4] = (x0 * (y1 * y3 - y2 * y3) + x1 * y2 * y3 -
		x2 * y1 * y3 +
		y0 * (x3 * y2 - x1 * y2 + (x2 - x3) * y1)) * hden;
	c[5] = y0;
	c[6] = (x1 * (y3 - y2) + x0 * (y2 - y3) +
		(x2 - x3) * y1 + (x3 - x2) * y0) * wden;
	c[7] = (-x2 * y3 + x1 * y3 + x3 * y2 +
		x0 * (y1 - y2) - x3 * y1 + (x2 - x1) * y0) * hden;
}

static void perspective_unmap(const double *c, double x, double y,
			      double *u, double *v)
{
	double den = 1.0 /
		(-c[0] * c[7] * y + c[1] * c[6] * y +
		 (c[3] * c[7] - c[4] * c[6]) * x +
		 c[0] * c[4] - c[1] * c[3]);

	*u = -(c[1] * (y - c[5]) - c[2] * c[7] * y +
	       (c[5] * c[7] - c[4]) * x + c[2] * c[4]) * den;
	*v = (c[0] * (y - c[5]) - c[2] * c[6] * y +
	      (c[5] * c[6] - c[3]) * x + c[2] * c[3]) * den;
}

static int defocus_blur(uint8_t *image, int width, int height, int passes)
{
	static const int weights[5] = {1, 4, 6, 4, 1};
	uint8_t *tmp;
	int pass;
	int x;
	int y;

	if (passes <= 0)
		return 0;
	tmp = malloc((size_t)width * height);
	if (!tmp)
		return -1;

	for (pass = 0; pass < passes; pass++) {
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				int sum = 0;
				int k;

				for (k = -2; k <= 2; k++) {
					int sx = x + k;

					if (sx < 0)
						sx = 0;
					if (sx >= width)
						sx = width - 1;
					sum += image[y * width + sx] *
						weights[k + 2];
				}
				tmp[y * width + x] = (uint8_t)((sum + 8) / 16);
			}
		}
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				int sum = 0;
				int k;

				for (k = -2; k <= 2; k++) {
					int sy = y + k;

					if (sy < 0)
						sy = 0;
					if (sy >= height)
						sy = height - 1;
					sum += tmp[sy * width + x] *
						weights[k + 2];
				}
				image[y * width + x] =
					(uint8_t)((sum + 8) / 16);
			}
		}
	}
	free(tmp);
	return 0;
}

static int render_marker(uint8_t *image, int width, int height,
			 const struct quirc_point corners[4], int framed,
			 int dark, int light, int blur_passes)
{
	double c[8];
	int x;
	int y;

	memset(image, light, (size_t)width * height);
	perspective_setup(c, corners, FRAME_UNITS, FRAME_UNITS);
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			double u;
			double v;

			perspective_unmap(c, x + 0.5, y + 0.5, &u, &v);
			if (marker_black(u, v, framed))
				image[y * width + x] = (uint8_t)dark;
		}
	}
	return defocus_blur(image, width, height, blur_passes);
}

static int decode_pixels(struct quirc *q, const uint8_t *source,
			 int width, int height,
			 enum quirc_marker_mode marker_mode,
			 int *identified)
{
	struct quirc_code code;
	struct quirc_data data;
	uint8_t *image;
	int count;
	int i;

	if (quirc_resize(q, width, height) != 0)
		return 0;
	quirc_set_marker_mode(q, marker_mode,
			      QUIRC_MARKER_PROFILE_OUTER_FRAME_V1);
	image = quirc_begin(q, NULL, NULL);
	memcpy(image, source, (size_t)width * height);
	quirc_end(q);

	count = quirc_count(q);
	if (identified)
		*identified = count > 0;
	for (i = 0; i < count; i++) {
		quirc_extract(q, i, &code);
		if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
			quirc_flip(&code);
			if (quirc_decode(&code, &data) != QUIRC_SUCCESS)
				continue;
		}
		if (waybeam_qr_data_valid(&data) &&
		    data.payload_len == (int)strlen(expected_payload) &&
		    memcmp(data.payload, expected_payload,
			   strlen(expected_payload)) == 0)
			return 1;
	}
	return 0;
}

static int decode_case(struct quirc *q,
		       const struct benchmark_case *test,
		       enum quirc_marker_mode marker_mode, int framed)
{
	uint8_t *image = malloc((size_t)QUICK_IMAGE_W * QUICK_IMAGE_H);
	int result;

	if (!image)
		return 0;
	if (render_marker(image, QUICK_IMAGE_W, QUICK_IMAGE_H,
			  test->corners, framed, 0, 255, 0) != 0) {
		free(image);
		return 0;
	}
	result = decode_pixels(q, image, QUICK_IMAGE_W, QUICK_IMAGE_H,
			       marker_mode, NULL);
	free(image);
	return result;
}

static int test_format_validator(void)
{
	static const char command_payload[] = "CRES1080P60A0030";
	struct quirc_data data;

	memset(&data, 0, sizeof(data));
	data.version = 1;
	data.ecc_level = QUIRC_ECC_LEVEL_Q;
	data.data_type = QUIRC_DATA_TYPE_ALPHA;
	data.payload_len = (int)strlen(expected_payload);
	memcpy(data.payload, expected_payload, strlen(expected_payload));
	if (!waybeam_qr_data_valid(&data))
		return 0;

	data.version = 2;
	if (waybeam_qr_data_valid(&data))
		return 0;
	data.version = 1;
	data.ecc_level = QUIRC_ECC_LEVEL_M;
	if (waybeam_qr_data_valid(&data))
		return 0;
	data.ecc_level = QUIRC_ECC_LEVEL_Q;
	data.payload_len--;
	if (waybeam_qr_data_valid(&data))
		return 0;
	data.payload_len++;
	data.payload[4] = '_';
	if (waybeam_qr_data_valid(&data))
		return 0;
	data.payload[4] = '5';
	data.payload[0] = 'X';
	if (waybeam_qr_data_valid(&data))
		return 0;
	memcpy(data.payload, command_payload, strlen(command_payload));
	if (!waybeam_qr_data_valid(&data))
		return 0;
	data.data_type = QUIRC_DATA_TYPE_BYTE;
	if (waybeam_qr_data_valid(&data))
		return 0;
	data.data_type = QUIRC_DATA_TYPE_ALPHA;
	data.payload[3] = 's';
	if (waybeam_qr_data_valid(&data))
		return 0;
	return 1;
}

struct sweep_result {
	int cases;
	int stock;
	int marker_found;
	int marker;
};

static void build_stress_corners(struct quirc_point corners[4],
				 int marker_width, double ratio,
				 int rotation_deg, int direction)
{
	double half = marker_width * 0.5;
	double top_half = half * ratio;
	double shear = marker_width * (1.0 - ratio) * 0.08;
	double local[4][2] = {
		{-top_half + shear, -half},
		{top_half + shear, -half},
		{half, half},
		{-half, half}
	};
	double angle = (rotation_deg + direction * 90) *
		3.14159265358979323846 / 180.0;
	double cosine = cos(angle);
	double sine = sin(angle);
	int i;

	for (i = 0; i < 4; i++) {
		double x = local[i][0];
		double y = local[i][1];

		corners[i].x = (int)rint(EXTENDED_IMAGE_W * 0.5 +
					 x * cosine - y * sine);
		corners[i].y = (int)rint(EXTENDED_IMAGE_H * 0.5 +
					 x * sine + y * cosine);
	}
}

static void sweep_add(struct sweep_result *result, int stock,
		      int marker_found, int marker)
{
	result->cases++;
	result->stock += stock;
	result->marker_found += marker_found;
	result->marker += marker;
}

static void print_sweep_result(const char *axis, const char *value,
			       const struct sweep_result *result)
{
	printf("  %-8s %-8s cases=%3d stock=%3d (%5.1f%%) "
	       "found=%3d (%5.1f%%) marker=%3d (%5.1f%%)\n",
	       axis, value, result->cases, result->stock,
	       result->cases ? 100.0 * result->stock / result->cases : 0.0,
	       result->marker_found,
	       result->cases ?
	       100.0 * result->marker_found / result->cases : 0.0,
	       result->marker,
	       result->cases ? 100.0 * result->marker / result->cases : 0.0);
}

static int run_extended_benchmark(struct quirc *q)
{
	static const int widths[] = {180, 250, 350, 500};
	static const int ratios[] = {100, 70, 50, 35};
	static const int rotations[] = {0, 35, 50};
	static const int blur_passes[] = {0, 1, 2, 3};
	struct sweep_result by_width[4] = {{0}};
	struct sweep_result by_ratio[4] = {{0}};
	struct sweep_result by_blur[4] = {{0}};
	struct sweep_result by_width_ratio[4][4] = {{{0}}};
	struct sweep_result by_ratio_blur[4][4] = {{{0}}};
	struct sweep_result overall = {0};
	uint8_t *image;
	clock_t started = clock();
	int wi, ri, ai, bi, direction;

	image = malloc((size_t)EXTENDED_IMAGE_W * EXTENDED_IMAGE_H);
	if (!image) {
		fprintf(stderr, "FAIL: extended corpus image allocation\n");
		return 1;
	}

	printf("extended corpus: Version-1/Q, 768 framed renders, "
	       "1280x720 source\n");
	for (wi = 0; wi < 4; wi++) {
		for (ri = 0; ri < 4; ri++) {
			for (ai = 0; ai < 3; ai++) {
				for (bi = 0; bi < 4; bi++) {
					for (direction = 0; direction < 4;
					     direction++) {
						struct quirc_point corners[4];
						int stock;
						int marker_found;
						int marker;

						build_stress_corners(corners,
							widths[wi],
							ratios[ri] / 100.0,
							rotations[ai],
							direction);
						if (render_marker(image,
							EXTENDED_IMAGE_W,
							EXTENDED_IMAGE_H,
							corners, 1, 0, 255,
							blur_passes[bi]) != 0) {
							free(image);
							fprintf(stderr,
								"FAIL: blur allocation\n");
							return 1;
						}
						stock = decode_pixels(q, image,
							EXTENDED_IMAGE_W,
							EXTENDED_IMAGE_H,
							QUIRC_MARKER_OFF,
							NULL);
						marker = decode_pixels(q, image,
							EXTENDED_IMAGE_W,
							EXTENDED_IMAGE_H,
							QUIRC_MARKER_ONLY,
							&marker_found);
						sweep_add(&overall,
							  stock,
							  marker_found,
							  marker);
						sweep_add(&by_width[wi],
							  stock,
							  marker_found,
							  marker);
						sweep_add(&by_ratio[ri],
							  stock,
							  marker_found,
							  marker);
						sweep_add(&by_blur[bi],
							  stock,
							  marker_found,
							  marker);
						sweep_add(
							&by_width_ratio[wi][ri],
							stock, marker_found,
							marker);
						sweep_add(
							&by_ratio_blur[ri][bi],
							stock, marker_found,
							marker);
					}
				}
			}
		}
		printf("  completed marker width %d px\n", widths[wi]);
		fflush(stdout);
	}
	free(image);

	printf("extended results by marker width:\n");
	for (wi = 0; wi < 4; wi++) {
		char value[16];

		snprintf(value, sizeof(value), "%dpx", widths[wi]);
		print_sweep_result("width", value, &by_width[wi]);
	}
	printf("extended results by compressed-edge ratio:\n");
	for (ri = 0; ri < 4; ri++) {
		char value[16];

		snprintf(value, sizeof(value), "0.%02d", ratios[ri]);
		if (ratios[ri] == 100)
			strcpy(value, "1.00");
		print_sweep_result("ratio", value, &by_ratio[ri]);
	}
	printf("extended results by defocus level:\n");
	for (bi = 0; bi < 4; bi++) {
		char value[16];

		snprintf(value, sizeof(value), "%d-pass", blur_passes[bi]);
		print_sweep_result("blur", value, &by_blur[bi]);
	}
	printf("extended stock/marker success by width and edge ratio "
	       "(48 cases/cell):\n");
	printf("             ratio 1.00   ratio 0.70   ratio 0.50   "
	       "ratio 0.35\n");
	for (wi = 0; wi < 4; wi++) {
		printf("  %3dpx    ", widths[wi]);
		for (ri = 0; ri < 4; ri++)
			printf(" %2d/%-2d        ",
			       by_width_ratio[wi][ri].stock,
			       by_width_ratio[wi][ri].marker);
		putchar('\n');
	}
	printf("extended stock/marker success by edge ratio and defocus "
	       "(48 cases/cell):\n");
	printf("             blur 0       blur 1       blur 2       blur 3\n");
	for (ri = 0; ri < 4; ri++) {
		printf("  %s    ", ratios[ri] == 100 ? "1.00" :
		       ratios[ri] == 70 ? "0.70" :
		       ratios[ri] == 50 ? "0.50" : "0.35");
		for (bi = 0; bi < 4; bi++)
			printf(" %2d/%-2d        ",
			       by_ratio_blur[ri][bi].stock,
			       by_ratio_blur[ri][bi].marker);
		putchar('\n');
	}
	print_sweep_result("overall", "all", &overall);
	printf("extended elapsed: %.2f CPU seconds\n",
	       (double)(clock() - started) / CLOCKS_PER_SEC);

	if (overall.marker_found != overall.cases ||
	    overall.marker * 100 < overall.cases * 90 ||
	    by_ratio[0].marker != by_ratio[0].cases ||
	    by_ratio[1].marker != by_ratio[1].cases ||
	    overall.marker < overall.stock) {
		fprintf(stderr,
			"FAIL: marker path regressed in extended stress corpus\n");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct quirc *q = quirc_new();
	int extended = argc == 2 && strcmp(argv[1], "--extended") == 0;
	int stock_pass = 0;
	int marker_pass = 0;
	int bare_stock;
	int bare_bounded;
	int framed_bounded;
	size_t i;

	if (!q) {
		fprintf(stderr, "FAIL: quirc allocation\n");
		return 1;
	}
	if (!test_format_validator()) {
		fprintf(stderr, "FAIL: minimal envelope validator\n");
		quirc_destroy(q);
		return 1;
	}
	if (argc > 2 || (argc == 2 && !extended)) {
		fprintf(stderr, "usage: %s [--extended]\n", argv[0]);
		quirc_destroy(q);
		return 2;
	}

	for (i = 0; i < sizeof(benchmark_cases) / sizeof(benchmark_cases[0]); i++) {
		int stock = decode_case(q, &benchmark_cases[i],
					QUIRC_MARKER_OFF, 1);
		int marker = decode_case(q, &benchmark_cases[i],
					 QUIRC_MARKER_ONLY, 1);

		stock_pass += stock;
		marker_pass += marker;
		printf("%-20s stock=%s marker=%s\n", benchmark_cases[i].name,
		       stock ? "PASS" : "MISS", marker ? "PASS" : "MISS");
	}
	bare_stock = decode_case(q, &benchmark_cases[0], QUIRC_MARKER_OFF, 0);
	bare_bounded = decode_case(q, &benchmark_cases[0],
				   QUIRC_MARKER_ONLY, 0);
	framed_bounded = decode_case(q, &benchmark_cases[4],
				     QUIRC_MARKER_ONLY, 1);

	printf("bounded-required     bare=%s framed=%s\n",
	       bare_bounded ? "UNEXPECTED" : "REJECTED",
	       framed_bounded ? "PASS" : "MISS");
	printf("summary: stock=%d/%zu marker=%d/%zu bounded=%s "
		       "format=PASS\n",
	       stock_pass,
	       sizeof(benchmark_cases) / sizeof(benchmark_cases[0]),
	       marker_pass,
	       sizeof(benchmark_cases) / sizeof(benchmark_cases[0]),
	       bare_stock && !bare_bounded && framed_bounded ? "PASS" : "FAIL");
	if (marker_pass !=
		    (int)(sizeof(benchmark_cases) / sizeof(benchmark_cases[0])) ||
	    !bare_stock || bare_bounded || !framed_bounded) {
		fprintf(stderr, "FAIL: marker-assisted perspective corpus\n");
		quirc_destroy(q);
		return 1;
	}
	if (extended) {
		int result = run_extended_benchmark(q);

		quirc_destroy(q);
		return result;
	}
	quirc_destroy(q);
	return 0;
}
