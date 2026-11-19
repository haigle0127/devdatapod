//! The built-in download catalog for the 软件管理 page.
//!
//! Official Windows archives only. The file name suffix decides how the archive is unpacked,
//! and the installer locates the server binary inside the archive and flattens it to
//! `Extensions/<kind><version>/` — nginx and MySQL ship a single top-level directory, and the
//! Redis 5 tarball tucks the binary into a `redis5/` subdirectory.

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Component {
    /// Unique key, e.g. `redis-8.10.1`.
    pub id: String,
    /// `"redis" | "mysql" | "nginx"`.
    pub kind: String,
    pub name: String,
    pub version: String,
    pub description: String,
    pub url: String,
    /// On-disk file name; the suffix selects zip vs tar.gz.
    pub file_name: String,
    /// Display only; 0 means unknown.
    pub size_bytes: u64,
}

impl Component {
    pub fn archive_kind(&self) -> ArchiveKind {
        if self.file_name.to_ascii_lowercase().ends_with(".tar.gz")
            || self.file_name.to_ascii_lowercase().ends_with(".tgz")
        {
            ArchiveKind::TarGz
        } else {
            ArchiveKind::Zip
        }
    }

    /// The server binary the flatten step looks for.
    pub fn server_binary(&self) -> &'static str {
        match self.kind.as_str() {
            "redis" => "redis-server.exe",
            "mysql" => "mysqld.exe",
            "nginx" => "nginx.exe",
            _ => "",
        }
    }

    pub fn size_label(&self) -> String {
        if self.size_bytes == 0 {
            return "未知大小".into();
        }
        const MB: f64 = 1024.0 * 1024.0;
        if self.size_bytes as f64 >= MB {
            format!("{:.1} MB", self.size_bytes as f64 / MB)
        } else {
            format!("{:.0} KB", self.size_bytes as f64 / 1024.0)
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArchiveKind {
    Zip,
    TarGz,
}

fn component(
    id: &str,
    kind: &str,
    name: &str,
    version: &str,
    description: &str,
    url: &str,
    file_name: &str,
    size_bytes: u64,
) -> Component {
    Component {
        id: id.into(),
        kind: kind.into(),
        name: name.into(),
        version: version.into(),
        description: description.into(),
        url: url.into(),
        file_name: file_name.into(),
        size_bytes,
    }
}

pub fn catalog() -> Vec<Component> {
    vec![
        component(
            "nginx-1.30.6",
            "nginx",
            "Nginx 1.30.6",
            "1.30.6",
            "高性能 HTTP 服务器与反向代理，官方 Windows 版",
            "https://nginx.org/download/nginx-1.30.6.zip",
            "nginx-1.30.6.zip",
            2_773_293,
        ),
        component(
            "redis-8.10.1",
            "redis",
            "Redis 8.10.1",
            "8.10.1",
            "内存键值数据库，Windows msys2 移植版",
            "https://github.com/redis-windows/redis-windows/releases/download/8.10.1/Redis-8.10.1-Windows-x64-msys2-with-Service.zip",
            "Redis-8.10.1.zip",
            20_341_313,
        ),
        component(
            "redis-7.4.11",
            "redis",
            "Redis 7.4.11",
            "7.4.11",
            "内存键值数据库，Windows msys2 移植版",
            "https://github.com/redis-windows/redis-windows/releases/download/7.4.11/Redis-7.4.11-Windows-x64-msys2-with-Service.zip",
            "Redis-7.4.11.zip",
            17_904_389,
        ),
        component(
            "redis-6.2.24",
            "redis",
            "Redis 6.2.24",
            "6.2.24",
            "内存键值数据库，Windows msys2 移植版",
            "https://github.com/redis-windows/redis-windows/releases/download/6.2.24/Redis-6.2.24-Windows-x64-msys2-with-Service.zip",
            "Redis-6.2.24.zip",
            17_988_288,
        ),
        component(
            "redis-5.0.7",
            "redis",
            "Redis 5.0.7",
            "5.0.7",
            "内存键值数据库，Windows cygwin 移植版",
            "https://github.com/redis-windows/redis-windows/releases/download/5.0.7/Redis-5.0.7-Windows-x64.tar.gz",
            "Redis-5.0.7.tar.gz",
            8_723_636,
        ),
        component(
            "mysql-8.0.46",
            "mysql",
            "MySQL 8.0.46",
            "8.0.46",
            "关系型数据库，主流稳定版",
            "https://cdn.mysql.com//Downloads/MySQL-8.0/mysql-8.0.46-winx64.zip",
            "mysql-8.0.46-winx64.zip",
            248_009_349,
        ),
        component(
            "mysql-8.4.11",
            "mysql",
            "MySQL 8.4.11",
            "8.4.11",
            "关系型数据库，LTS 长期支持版",
            "https://cdn.mysql.com//Downloads/MySQL-8.4/mysql-8.4.11-winx64.zip",
            "mysql-8.4.11-winx64.zip",
            281_191_914,
        ),
        component(
            "mysql-9.7.2",
            "mysql",
            "MySQL 9.7.2",
            "9.7.2",
            "关系型数据库，最新创新版",
            "https://cdn.mysql.com//Downloads/MySQL-9.7/mysql-9.7.2-winx64.zip",
            "mysql-9.7.2-winx64.zip",
            342_737_674,
        ),
        component(
            "mysql-5.7.44",
            "mysql",
            "MySQL 5.7.44",
            "5.7.44",
            "关系型数据库，经典老版本",
            "https://cdn.mysql.com/archives/mysql-5.7/mysql-5.7.44-winx64.zip",
            "mysql-5.7.44-winx64.zip",
            352_891_656,
        ),
    ]
}

pub fn find(id: &str) -> Option<Component> {
    catalog().into_iter().find(|item| item.id == id)
}
