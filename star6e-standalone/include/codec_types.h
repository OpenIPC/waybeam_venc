#ifndef CODEC_TYPES_H
#define CODEC_TYPES_H

/* Shared codec payload enum retained for the current backend code paths. */
typedef enum {
	PT_H264 = 0,
	PT_H265 = 1,
} PAYLOAD_TYPE_E;

#endif /* CODEC_TYPES_H */
