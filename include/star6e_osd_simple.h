#ifndef STAR6E_OSD_SIMPLE_H
#define STAR6E_OSD_SIMPLE_H

#define STAR6E_OSD_DOT_W 8u
#define STAR6E_OSD_DOT_H 8u
#define STAR6E_OSD_TRACK_POINT_COUNT 10u
#define STAR6E_OSD_TRACK_POINT_W 4u
#define STAR6E_OSD_TRACK_POINT_H 4u
#define STAR6E_OSD_COLOR_RED 1u
#define STAR6E_OSD_COLOR_TRANSPARENT 15u
#define STAR6E_OSD_COLOR_WHITE 7u
#define STAR6E_OSD_COLOR_GREEN 2u

/** Select the channel/port that the marker OSD should attach to. */
int star6e_osd_set_vpe_target(const void *chn_port);

/** Configure the full-screen OSD canvas dimensions before region creation. */
int star6e_osd_set_canvas_size(unsigned int width, unsigned int height);

/** Create and attach the Star6E optical-flow marker OSD region. */
int star6e_osd_add_dot_region(unsigned int x, unsigned int y,
	unsigned int color);

/** Move the Star6E optical-flow marker OSD region. */
int star6e_osd_move_dot_region(unsigned int x, unsigned int y,
	unsigned int color);

/** Update the debug LK point overlay positions on the OSD canvas. */
int star6e_osd_set_track_points(const unsigned int *x_points,
	const unsigned int *y_points, unsigned int count);

/** Detach and destroy the Star6E optical-flow marker OSD region. */
int star6e_osd_remove_dot_region(void);

#endif /* STAR6E_OSD_SIMPLE_H */