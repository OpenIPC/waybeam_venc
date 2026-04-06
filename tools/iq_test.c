#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    void *h = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    /* Try MI_ISP_IQ_SetAll bulk API */
    typedef int (*setall_fn)(uint32_t, uint32_t, uint16_t, uint32_t, uint8_t*);
    setall_fn fn_setall = (setall_fn)dlsym(h, "MI_ISP_IQ_SetAll");

    typedef int (*set_fn)(uint32_t, uint32_t, void*);
    typedef int (*get_fn)(uint32_t, uint32_t, void*);

    /* ColorToGray = API ID 4100 */
    printf("=== ColorToGray ===\n");
    uint32_t enable = 1;
    if (fn_setall) {
        int r = fn_setall(0, 0, 4100, sizeof(enable), (uint8_t*)&enable);
        printf("SetAll(4100, enable=1) ret=%d (0x%x)\n", r, r);
    }
    set_fn fn_ctg_set = (set_fn)dlsym(h, "MI_ISP_IQ_SetColorToGray");
    if (fn_ctg_set) {
        int r = fn_ctg_set(0, 0, &enable);
        printf("SetColorToGray(1) ret=%d (0x%x)\n", r, r);
    }
    get_fn fn_ctg_get = (get_fn)dlsym(h, "MI_ISP_IQ_GetColorToGray");
    if (fn_ctg_get) {
        uint32_t val = 99;
        int r = fn_ctg_get(0, 0, &val);
        printf("GetColorToGray ret=%d val=%u\n", r, val);
    }

    /* Brightness = API ID 4102 */
    printf("=== Brightness ===\n");
    uint8_t bri_buf[256];
    get_fn fn_bri_get = (get_fn)dlsym(h, "MI_ISP_IQ_GetBrightness");
    set_fn fn_bri_set = (set_fn)dlsym(h, "MI_ISP_IQ_SetBrightness");
    if (fn_bri_get && fn_bri_set) {
        memset(bri_buf, 0, sizeof(bri_buf));
        int r = fn_bri_get(0, 0, bri_buf);
        uint32_t en, op, val;
        memcpy(&en, bri_buf, 4);
        memcpy(&op, bri_buf+4, 4);
        memcpy(&val, bri_buf+72, 4);
        printf("Get ret=%d en=%u op=%u val@72=%u\n", r, en, op, val);

        /* Set to manual mode, value=0 (dark) */
        en = 1; op = 1; val = 0;
        memcpy(bri_buf, &en, 4);
        memcpy(bri_buf+4, &op, 4);
        memcpy(bri_buf+72, &val, 4);
        r = fn_bri_set(0, 0, bri_buf);
        printf("Set(en=1,op=1,val=0) ret=%d (0x%x)\n", r, r);

        /* Read back */
        memset(bri_buf, 0, sizeof(bri_buf));
        r = fn_bri_get(0, 0, bri_buf);
        memcpy(&en, bri_buf, 4);
        memcpy(&op, bri_buf+4, 4);
        memcpy(&val, bri_buf+72, 4);
        printf("Get after set: ret=%d en=%u op=%u val@72=%u\n", r, en, op, val);
    }

    dlclose(h);
    return 0;
}
