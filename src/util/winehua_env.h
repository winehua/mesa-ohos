/*
 * WineHua 环境开关的**值感知**判定 (与 wine 侧 winehua_env_enabled 同语义:
 * dlls/winewayland.drv/opengl_diag.c)。
 *
 * 为什么需要它 (2026-10-01 review 实测): mesa 侧原先只看变量**在不在**
 * (`getenv("WINEHUA_VTEST_PRESENT") != NULL`), 而 wine 侧
 * (dlls/winex11.drv/opengl_winehua.c) 把 "0" 当关。于是置 "0" 做对照实验时
 * wine 不装私有驱动 (回 win32u 通用 EGL, swap 空实现), mesa 却因"变量存在"
 * 跳过上游 transfer_get + displaytarget_display 呈现路径 ⇒ 两头都不出图,
 * 唯一提示是一行 WARN。两端必须同一套语义: 未设/空/"0" = 关。
 */
#ifndef WINEHUA_ENV_H
#define WINEHUA_ENV_H

#include <stdlib.h>
#include <string.h>

static inline int winehua_env_enabled(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] && strcmp(value, "0") != 0;
}

#endif /* WINEHUA_ENV_H */
