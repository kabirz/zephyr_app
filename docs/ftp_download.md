
# ftp 下载

## 1. 主动模式下载
```shell
curl -O -u admin:admin ftp://192.168.12.101/data_1733339857.raw --ftp-port 192.168.12.10 --disable-eprt
```

## 2. 拓展主动模式下载
```shell
curl -O -u admin:admin ftp://192.168.12.101/data_1733339857.raw --ftp-port 192.168.12.10
```

## 3. 主动模式下载
```shell
curl -O -u admin:admin ftp://192.168.12.101/data_1733339857.raw --ftp-pasv --disable-epsv
```

## 4. 拓展被动模式下载
```shell
curl -O -u admin:admin ftp://192.168.12.101/data_1733339857.raw --ftp-pasv
```

## 5. 上传文件
```shell
curl -u admin:admin -T test.txt ftp://192.168.12.101
```
