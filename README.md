# QR Blocker Filter - OBS Plugin

一个用于自动检测并遮挡视频中二维码的 OBS 插件。

## 功能

- 自动检测视频中的二维码
- 支持使用源（Source B）或纯色遮挡二维码
- 支持自定义检测间隔和检测区域
- 提供快捷键控制（自动/强制遮挡/临时隐藏）

## 安装

1. 下载最新的 `qr-blocker-filter.zip`
2. 解压到 OBS 安装目录（默认：`C:\Program Files\obs-studio\`）
3. 重启 OBS

## 快捷键

| 状态 | 按热键 | 结果 |
|------|--------|------|
| 自动模式 + 无二维码 | 强制遮挡 |
| 自动模式 + 有二维码 | 临时隐藏 130s |
| 强制遮挡 + 无二维码 | 回到自动模式 |
| 强制遮挡 + 有二维码 | 临时隐藏 130s |
| 临时隐藏中 | 立即取消隐藏 |

## 构建

要求：Visual Studio 2022

```powershell
.\rebuild.cmd
```

## 许可证

MIT License
