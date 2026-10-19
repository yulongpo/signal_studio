# Signal Studio 品牌与 Qt 工程资源包 · v2.0

## 设计定位
基于 RF 信号幅度谱与 FFT 条形谱的融合符号，深蓝底、青蓝双渐变；三类商标均为**原生 SVG 几何矢量路径**，不是将 PNG 包装为 SVG，也不是像素轮廓自动追踪。

- `svg/icon-*.svg` 独立图形徽标
- `svg/wordmark-*.svg` 独立 Signal Studio 字标
- `svg/lockup-*.svg` 横向组合徽标
- `png/` 常用 PNG 导出，均为 SVG 渲染
- `qt/resources/branding.qrc` Qt 资源清单
- `qt/windows/app_icon.rc` 与 `signal_studio.ico` Windows EXE 图标

每一类均包含 `primary`（主色版）、`dark`（深色界面）、`light`（浅色界面）、`mono-dark`（深色单色）、`mono-light`（浅色单色）。

> 注：此处 `mono-dark` 是**深色图形**用于浅底，`mono-light` 是**浅色图形**用于深底。主色徽标自带深蓝圆角矩形底，其外部透明。

## Qt 6.11.1 / CMake 接入

将本文件夹复制到工程中，如 `resources/SignalStudioBrand/`（路径可自定）。在已有的 `SignalStudio` CMake target **定义完成后**添加：

```cmake
set(CMAKE_AUTORCC ON)
# 如果 CMAKE_AUTORCC 在目标创建后才设置，请改为设置目标属性：
set_target_properties(SignalStudio PROPERTIES AUTORCC ON)

target_sources(SignalStudio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/SignalStudioBrand/qt/resources/branding.qrc
)

if (WIN32)
    target_sources(SignalStudio PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/SignalStudioBrand/qt/windows/app_icon.rc
    )
endif()
```

**Qt Widgets 中使用：**

```cpp
#include <QIcon>
#include <QLabel>
#include <QPixmap>

setWindowIcon(QIcon(":/branding/icon-primary.svg"));

// 例如欢迎界面的组合 Logo；实际控件与尺寸可按当前 UI 适配。
auto* brandLabel = new QLabel(this);
brandLabel->setPixmap(QIcon(":/branding/lockup-dark.svg").pixmap(360, 80));
```

Qt 需确保部署 `qsvg` 图像格式插件（如系统通过 Qt SVG 处理 SVG），或者 QtSvg 相关渲染组件可用。需要最稳妥的无额外 SVG 插件回退时，可使用 `:/branding/raster/signal_studio_icon_256.png`。

```cpp
setWindowIcon(QIcon(":/branding/raster/signal_studio_icon_256.png"));
```

对于 Windows 任务栏和文件管理器，`qt/windows/signal_studio.ico` 通过 `app_icon.rc` 编译进 EXE；窗口运行时图标通过 `setWindowIcon` 配置，二者用途不同。

## 使用场景

| 位置 | 推荐文件 | 建议 |
|---|---|---|
| Windows EXE / .ico | `qt/windows/signal_studio.ico` | 包含 16–256 px 多尺度图层 |
| Qt 标题栏 / 任务栏 | `svg/icon-primary.svg` | 可选 PNG 256 回退 |
| 深色界面欢迎页 | `svg/lockup-dark.svg` | 图形+文字 |
| 浅色界面欢迎页 | `svg/lockup-light.svg` | 图形+文字 |
| 顶部应用标题 | `svg/wordmark-dark.svg` | 纯文字 |
| 单色打印 / 反白 | `svg/*-mono-dark.svg` 或 `*-mono-light.svg` | 全路径、无需字体 |

## 规格及可编辑性

- 所有 SVG 无外链图片、无外部字体、无 JavaScript，文字已轮廓化为矢量路径。
- 文本路径基于可自由使用的 Lato 字型轮廓构建；ZIP 内**不包含字体文件**。
- Illustrator、Inkscape 和 Qt SVG 均可消费常用 path、rect、linearGradient 等基础 SVG 元素。
- 需要编辑 Logo 形状或颜色时修改 SVG；不建议对 PNG 或 ICO 再做无损矢量化。
- 图标 16 px 下 FFT 栅格细节会自然减少；Windows 图标所有分辨率均从矢量母版分别渲染。
