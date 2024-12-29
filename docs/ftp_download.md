
# ftp 下载

## 1. 主动模式下载
```shell
curl -O ftp://192.168.12.101/data_1733339857.raw --ftp-port 192.168.12.10 --disable-eprt
```

## 2. 拓展主动模式下载
```shell
curl -O ftp://192.168.12.101/data_1733339857.raw --ftp-port 192.168.12.10
```

## 3. 主动模式下载
```shell
curl -O ftp://192.168.12.101/data_1733339857.raw --ftp-pasv --disable-epsv
```

## 4. 拓展被动模式下载
```shell
curl -O ftp://192.168.12.101/data_1733339857.raw --ftp-pasv
```

## 5. 上传文件
需要使用admin账号才有权限，匿名账号不支持上传文件
```shell
curl -u admin:admin -T test.txt ftp://192.168.12.101
```

## 使用wget下载

```shell
# 下载单个文件
wget ftp://192.168.12.101/data_1735428222.raw
# 下载整个目录, 下载完后保存在目录192.168.12.101下
wget ftp://192.168.12.101/ -r
```

## 注意事项

在Linux 环境下使用ftp命令进入交互模式下载文件时可能会出现"Reading from network: Interrupted system call"的错，导致下载失败,
出现这个问题的原因是ftp交互模式下默认使用的是ascii模式下载，在这个模式下linux/unix对回车换行解析时可能出错。解决方法是设置为binary模式，直接输入bin命令切换，这时候使用get命令下载文件就行了。

使用wget或者curl工具时默认会切换到binary模式下载不会有这个问题。

