# 环境搭建

本项目基于 [Zephyr RTOS](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) 开发，使用 [west](https://docs.zephyrproject.org/latest/develop/west/index.html) 管理多仓库与构建。

## 工具链版本

| 工具 | 要求版本 |
|------|----------|
| Zephyr SDK | `0.16.8` 及以上 |
| Python | `3.12` 及以上 |
| 构建工具 | `west`（随 Python 包安装） |
| CMake | `>= 3.20` |

## Linux 环境

### 1. 安装 Zephyr SDK 与依赖

参考 Zephyr 官方 [Getting Started](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) 安装系统依赖与 SDK（`arm-zephyr-eabi` 工具链）。

### 2. 创建 Python 虚拟环境

```bash
python3 -m venv venv
source venv/bin/activate
pip install west
```

### 3. 获取源码

```bash
west init -m https://github.com/kabirz/zephyr_app app
cd app
west update
west package pip --install
```

### 4. 应用补丁（如使用 laser 分支）

```bash
git -C ../zephyr am "$(pwd)/patches/zephyr_*"
```

## Windows 环境

Windows 环境推荐使用 [uv](https://docs.astral.sh/uv/) 管理 Python，详见 [Windows 环境配置](setup_windows.md)。

!!! tip "提示"
    `west init` 时通过 `--mf` 指定不同的 manifest 可切换 Zephyr 版本，例如激光应用使用 `zephyr4_3.yml`：

    ```bash
    west init -m https://github.com/kabirz/zephyr_app app --mf zephyr4_3.yml
    ```

## 下一步

环境就绪后，前往 [构建与烧录](build-flash.md) 开始构建固件。
