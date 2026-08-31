# EdgeNote

一个面向 Windows 10 / Windows 11 的轻量桌面便签工具，使用原生 Win32 C 编写。

## 当前功能

- 真正无系统边框的便签外观
- 一个程序内可创建多个独立便签
- 每个便签都可以独立输入文字
- 每个便签可独立设置颜色
- 新建便签自动轮换预设颜色
- 支持黄色、粉色、蓝色、绿色、紫色、橙色
- 支持 Windows 自带取色器自定义颜色
- 支持左 / 右 / 上 / 下四边贴边
- 贴边后自动隐藏
- 鼠标靠近对应屏幕边缘时自动出现
- 窗口可拖动、可从四边和四角调整大小
- 支持总在最前
- 支持字体放大 / 缩小

## 下载 EXE

每次更新 `src/EdgeNote.c` 后，GitHub Actions 会自动编译 Windows x64 版本。

打开仓库顶部的 **Actions** → 最新一次 **Build EdgeNote EXE** → 页面底部 **Artifacts** → 下载 `EdgeNote-Win10-x64`。

解压后运行 `EdgeNote.exe` 即可，不需要安装 Python。

> Windows SmartScreen 可能提示“未知发布者”，因为该 EXE 没有商业代码签名证书。

## 项目结构

```text
EdgeNote/
├─ src/
│  └─ EdgeNote.c
├─ .github/
│  └─ workflows/
│     └─ build.yml
└─ README.md
```

## 后续计划

- 自动保存每个便签的内容
- 记忆每个便签的位置、大小和颜色
- 程序重新启动后恢复全部便签
- 区分“关闭便签”和“永久删除便签”
