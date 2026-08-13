/*
 * imx662_cmos_param.h — IMX662 ISP tuning seeds for Hi3516CV610.
 *
 * The active AWB calibration was recovered from the board factory xipc's
 * IMX662_ISP_SetAWBAttr() implementation. The older FRAMOS/Verisilicon CCM
 * seeds remain below as documentation only; hardware testing showed they are
 * not directly portable to this HiSilicon optical/ISP path.
 *
 * HiSilicon fixed-point conventions used here:
 *   - WB gain:  256 == 1.0x   (matches the vendor sensor drivers' scale)
 *   - CCM coeff: magnitude * 256, bit 15 set for a negative coefficient
 *   - Black level: on the pipeline bit depth (12-bit here)
 */

#ifndef IMX662_CMOS_PARAM_H
#define IMX662_CMOS_PARAM_H

/* Sensor id used in the ISP/AE/AWB register callbacks.
 * TODO: match the value the SDK/app uses in getSnsObj()/enum for IMX662. */
#ifndef IMX662_ID
#define IMX662_ID   662
#endif

/* ---- Black level (FRAMOS blsData = 200/4095 on all 4 channels, 12-bit) ----
 * Scale to your ISP path's bit depth if not 12-bit (200 @12b ≈ 50 @10b ≈ 12 @8b).
 */
#define IMX662_BLACK_LEVEL_12BIT   200

/* ---- AWB static reference and Planckian curve -----------------------------
 * Mode 0/1 values recovered verbatim from the factory xipc. The scene routine
 * leaves ref_color_temp unchanged; 4950 K follows the CV6xx sensor-driver
 * convention until the factory sensor callback itself is recovered.
 */
#define IMX662_AWB_STATIC_TEMP     4950
#define IMX662_AWB_STATIC_WB_R     418
#define IMX662_AWB_STATIC_WB_GR    256           /* 1.000 * 256 */
#define IMX662_AWB_STATIC_WB_GB    256           /* 1.000 * 256 */
#define IMX662_AWB_STATIC_WB_B     545
#define IMX662_AWB_P1              (-86)
#define IMX662_AWB_P2              342
#define IMX662_AWB_Q1              0
#define IMX662_AWB_A1              199146
#define IMX662_AWB_B1              128
#define IMX662_AWB_C1              (-148048)

/* ---- Inactive reference CCMs (FRAMOS, row-major 3x3) ----------------------
 * Reference float values (transcribe/convert to the exact HiSilicon
 * ot_isp_cmos_ccm sign+fixed-point format when wiring cmos_get_isp_default):
 *
 *  Illum A  (~2856K):  1.43277  0.02499 -0.44446
 *                     -0.53870  1.76311 -0.19127
 *                      0.07976 -0.85214  1.78090
 *
 *  D65 (~6500K):       1.71817 -0.42729 -0.28413
 *                     -0.33666  1.66320 -0.30384
 *                     -0.00780 -0.41439  1.43250
 *
 *  F11/TL84:           1.62374 -0.31544 -0.30106
 *                     -0.49317  1.71976 -0.17621
 *                      0.01479 -0.54041  1.56820
 *
 *  F2/CWF:             2.08814 -0.84402 -0.23719
 *                     -0.46004  1.60443 -0.09964
 *                      0.00435 -0.48847  1.52720
 *
 * D65 pre-converted to *256 signed (drop-in starting CCM):
 */
#define IMX662_CCM_D65 { \
     440, -109,  -73,   \
     -86,  426,  -78,   \
      -2, -106,  367 }

#define IMX662_CCM_A { \
     367,    6, -114,   \
    -138,  451,  -49,   \
      20, -218,  456 }

#define IMX662_CCM_F11 { \
     416,  -81,  -77,   \
    -126,  440,  -45,   \
       4, -138,  401 }

#define IMX662_CCM_F2 { \
     535, -216,  -61,   \
    -118,  411,  -26,   \
       1, -125,  391 }

/* Color temperatures for the CCM/AWB anchors (Kelvin). */
#define IMX662_CT_A     2856
#define IMX662_CT_F2    4150
#define IMX662_CT_F11   4000
#define IMX662_CT_D65   6500

/*
 * TODO(on-device tuning), not derivable from FRAMOS/datasheet:
 *   - GAMMA / tone curve (FRAMOS only had a linear DEGAMMA identity ramp).
 *   - NR (2D/3D denoise), sharpen, DRC/WDR curve, DPC strength.
 *   - LSC lens-shading grid — LENS-SPECIFIC, must be captured per module
 *     (FRAMOS grids were flat/uncalibrated = unusable).
 *   - Final CCM after verifying HiSilicon's sign/format + saturation.
 */

#endif /* IMX662_CMOS_PARAM_H */
