# Brand v2 Integration

原始 Brand v2.0 SVG/PNG 复制到 `resources/branding`。SVG 路径与 PNG 保持原样；qrc 加入组合/文字 PNG 回退。原包 ICO 实有16/32/48/64/128/256六层，`scripts/pack-brand-icon.ps1` 将包内已有七个尺寸 PNG 无缩放封装成 ICO，补齐24px；`verify-brand.ps1` 读取 PE GROUP_ICON 验证。

`SignalStudioBrand` 静态库只编译一次 qrc，主程序、UI测试和捕获程序链接同一资源。全局资源初始化锚点防止静态库链接裁剪。BrandAssets 用类型/变体/逻辑尺寸/DPR/回退选项缓存 pixmap。运行窗口图标始终使用多尺寸PNG，不依赖SVG插件；展示优先SVG，读取失败自动PNG，`SS_BRAND_FORCE_PNG=1`用于独立包回退验收。

| 场景 | 资源 / 逻辑尺寸 |
|---|---|
| 欢迎工程入口 | lockup-dark / 360×90 |
| 自定义标题栏 | icon-primary 24 / wordmark-dark 132×24 |
| 导入文件条 | icon-primary 32，原型42px外框 |
| 窄带页头 | icon-primary 24 |
| 关于 | lockup-dark / 360×90、版本/构建SHA/Qt/编译器/许可说明、复制构建信息 |
| EXE / 任务栏 / Alt+Tab | PE ICO / QApplication与QWidget运行图标 |

`mono-dark`是深色图形用于浅底，`mono-light`为浅色图形用于深底。测试遍历所有15个SVG变体与三类PNG回退；不新增应用浅色主题。无品牌覆盖PSD、STFT、导航纹理或修改LUT。SVG无脚本、外链图片、文字节点；Lato文字已轮廓化，不分发字体。

Qt6.11.1采用动态链接，部署包含qsvg imageformats、qsvgicon和Qt6Svg。Qt许可说明入口见关于页；品牌版权依据资源原始README，仓库未另行声明整体产品开源许可。
