# MySQL 功能说明

## 安装依赖

Ubuntu 或 Debian 系统执行：

```sh
sudo apt update
sudo apt install default-libmysqlclient-dev libssl-dev pkg-config
```

也可以使用 MariaDB 开发包：

```sh
sudo apt install libmariadb-dev libssl-dev pkg-config
```

## 初始化数据库

使用拥有创建数据库权限的账户执行初始化脚本：

```sh
mysql -u root -p < sql/init.sql
```

服务器运行时建议使用专用数据库账户，并且只授予程序需要的查询和插入权限：

```sql
CREATE USER 'webserver'@'127.0.0.1' IDENTIFIED BY '请修改这个密码';
GRANT SELECT, INSERT ON mywebserver.* TO 'webserver'@'127.0.0.1';
FLUSH PRIVILEGES;
```

## 构建和运行

使用 MySQL 和 OpenSSL 支持构建程序：

```sh
make mysql
```

数据库参数通过环境变量配置，不要把数据库密码提交到仓库：

```sh
export MYSQL_ENABLED=1
export MYSQL_HOST=127.0.0.1
export MYSQL_PORT=3306
export MYSQL_USER=webserver
export MYSQL_PASSWORD=请修改这个密码
export MYSQL_DATABASE=mywebserver
export MYSQL_POOL_SIZE=8
./webserver -p 8080 -t 8
```

`MYSQL_POOL_SIZE` 默认等于工作线程数量。获取连接最长等待两秒，防止工作线程因为连接不足而永久阻塞。

普通的 `make build` 不启用 MySQL，可用于运行不依赖数据库的 HTTP 功能。如果使用普通版本并设置
`MYSQL_ENABLED=1`，程序会在启动时提示需要通过 `make mysql` 重新构建。

## 注册接口

```sh
curl -i -X POST http://127.0.0.1:8080/register \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data 'username=alice&password=请设置一个安全密码'
```

## 登录接口

```sh
curl -i -X POST http://127.0.0.1:8080/login \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data 'username=alice&password=请设置一个安全密码'
```

密码使用带随机盐的 PBKDF2-HMAC-SHA256 算法保存。SQL 参数全部通过预处理语句绑定，避免 SQL 注入。
用户名不存在和密码错误都会返回相同的 `401` 响应，避免泄露用户是否存在。

## 并发测试

保留上面的环境变量，然后执行：

```sh
make test-mysql
```

测试程序会针对同一个新用户名发出 48 个并发注册请求。最终必须只有一个请求返回 `201`，其余
47 个请求返回 `409`。并发正确性由 `users.username` 的唯一索引保证，而不是依赖进程内的先查询后插入。
