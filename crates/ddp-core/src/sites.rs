//! `data/sites.json` — local virtual hosts rendered into nginx.
//!
//! The store only validates and persists. Turning a site into an nginx `server` block is the
//! driver's job, because the port is whatever nginx was allocated, not a constant.

use std::path::{Path, PathBuf};

use serde_json::{Map, Value};

use crate::atomic;
use crate::log;

const SCHEMA_VERSION: i64 = 1;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Site {
    pub domain: String,
    pub aliases: Vec<String>,
    pub port: u16,
    /// Absolute or install-root-relative; the renderer resolves it.
    pub root_dir: String,
    /// `YYYY-MM-DD`, empty = never expires.
    pub expire_at: String,
    pub remark: String,
}

impl Site {
    pub fn path(&self) -> PathBuf {
        PathBuf::from(&self.root_dir)
    }

    /// True when the expiry date is before today. An empty date never expires.
    pub fn expired(&self) -> bool {
        if self.expire_at.len() < 10 {
            return false;
        }
        let today = chrono::Local::now().format("%Y-%m-%d").to_string();
        self.expire_at[..10] < today[..]
    }
}

pub fn file_path(root: &Path) -> PathBuf {
    root.join("data").join("sites.json")
}

pub fn load(root: &Path) -> (Vec<Site>, Option<String>) {
    let path = file_path(root);
    if !path.exists() {
        return (Vec::new(), None);
    }
    let raw = match atomic::read_bytes(&path) {
        Ok(raw) => raw,
        Err(error) => return (Vec::new(), Some(error)),
    };
    // The Qt build wrote this file with a UTF-8 BOM; serde_json rejects it.
    let raw = raw.strip_prefix(&[0xEF, 0xBB, 0xBF]).unwrap_or(&raw);
    let value: Value = match serde_json::from_slice(raw) {
        Ok(value) => value,
        Err(error) => {
            return (
                Vec::new(),
                Some(format!("{} 不是合法 JSON：{error}", path.display())),
            )
        }
    };
    let Some(items) = value.get("items").and_then(Value::as_array) else {
        return (Vec::new(), None);
    };

    let mut out = Vec::with_capacity(items.len());
    for item in items {
        let object = item.as_object().cloned().unwrap_or_default();
        let mut site = Site {
            domain: normalize(&text(&object, "domain")),
            port: object.get("port").and_then(Value::as_u64).unwrap_or(0) as u16,
            root_dir: text(&object, "rootDir"),
            expire_at: text(&object, "expireAt"),
            remark: text(&object, "remark"),
            ..Default::default()
        };
        if site.domain.is_empty() {
            continue;
        }
        if let Some(aliases) = object.get("aliases").and_then(Value::as_array) {
            for alias in aliases {
                let alias = normalize(alias.as_str().unwrap_or(""));
                if !alias.is_empty() && alias != site.domain && !site.aliases.contains(&alias) {
                    site.aliases.push(alias);
                }
            }
        }
        out.push(site);
    }
    (out, None)
}

pub fn save(root: &Path, items: &[Site]) -> Result<(), String> {
    let array: Vec<Value> = items
        .iter()
        .map(|site| {
            let mut object = Map::new();
            object.insert("domain".into(), Value::from(site.domain.clone()));
            object.insert(
                "aliases".into(),
                Value::Array(site.aliases.iter().cloned().map(Value::from).collect()),
            );
            object.insert("port".into(), Value::from(site.port));
            object.insert("rootDir".into(), Value::from(site.root_dir.clone()));
            object.insert("expireAt".into(), Value::from(site.expire_at.clone()));
            object.insert("remark".into(), Value::from(site.remark.clone()));
            Value::Object(object)
        })
        .collect();

    let mut document = Map::new();
    document.insert("schema".into(), Value::from(SCHEMA_VERSION));
    document.insert("items".into(), Value::Array(array));

    let path = file_path(root);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|error| format!("无法创建 {}：{error}", parent.display()))?;
    }
    let mut bytes = serde_json::to_vec_pretty(&Value::Object(document)).unwrap_or_default();
    bytes.push(b'\n');
    if !atomic::write_atomic(&path, &bytes) {
        return Err(format!("无法写入 {}", path.display()));
    }
    Ok(())
}

/// Validate a site against everything already configured. `self_domain` is the domain the
/// edited site had before the dialog opened, so renaming does not collide with itself.
pub fn validate(
    site: &Site,
    self_domain: Option<&str>,
    existing: &[Site],
) -> Result<(), String> {
    let domain = normalize(&site.domain);
    if !looks_like_host(&domain) {
        return Err("域名只填主机名，例如 www.example.com（不要带 http:// 或端口）".into());
    }
    if site.port == 0 {
        return Err("端口必须在 1-65535 之间".into());
    }
    if site.root_dir.trim().is_empty() {
        return Err("根目录不能为空".into());
    }

    let mut aliases: Vec<String> = Vec::new();
    for raw in &site.aliases {
        let alias = normalize(raw);
        if alias.is_empty() || alias == domain {
            continue;
        }
        if !looks_like_host(&alias) {
            return Err(format!("第二域名格式不对：{raw}"));
        }
        if aliases.contains(&alias) {
            return Err(format!("第二域名重复：{alias}"));
        }
        aliases.push(alias);
    }

    let self_key = self_domain.map(normalize).unwrap_or_default();
    for other in existing {
        if !self_key.is_empty() && normalize(&other.domain) == self_key {
            continue;
        }
        if normalize(&other.domain) == domain || other.aliases.contains(&domain) {
            return Err(format!("域名 {domain} 已经绑在别的站点上"));
        }
        if aliases.contains(&normalize(&other.domain)) {
            return Err(format!(
                "第二域名 {} 已经是另一个站点的主域名",
                normalize(&other.domain)
            ));
        }
        for alias in &aliases {
            if other.aliases.contains(alias) {
                return Err(format!("第二域名 {alias} 已经被别的站点占用"));
            }
        }
        if other.port == site.port {
            return Err(format!("端口 {} 已经被站点 {} 占用", site.port, other.domain));
        }
    }
    Ok(())
}

pub fn default_root_dir(root: &Path, domain: &str) -> String {
    root.join("WWW").join(normalize(domain)).display().to_string()
}

/// Create the directory and, only when it has no `index.*`, a placeholder page. Anything the
/// user put there wins.
pub fn prepare_root(site: &Site) -> Result<(), String> {
    let root = PathBuf::from(&site.root_dir);
    if root.as_os_str().is_empty() {
        return Err("根目录不能为空".into());
    }
    std::fs::create_dir_all(&root).map_err(|error| format!("无法创建目录 {}：{error}", root.display()))?;
    if root.join("index.html").exists() || root.join("index.htm").exists() {
        return Ok(());
    }
    let page = placeholder_page(site);
    if !atomic::write_atomic(&root.join("index.html"), page.as_bytes()) {
        // The directory existing is enough: nginx answers 403/404, deployment still works.
        log::warn(&format!("sites: 无法写入占位页 {}", root.join("index.html").display()));
    }
    Ok(())
}

pub fn find<'a>(items: &'a [Site], domain: &str) -> Option<&'a Site> {
    let key = normalize(domain);
    items.iter().find(|site| normalize(&site.domain) == key)
}

pub fn normalize(raw: &str) -> String {
    raw.trim().to_lowercase()
}

/// Hostnames only: `localhost` is allowed, `a-b.example.com` is allowed; a scheme, slash or
/// port is not.
pub fn looks_like_host(domain: &str) -> bool {
    if domain.is_empty() || domain.len() > 253 {
        return false;
    }
    for label in domain.split('.') {
        if label.is_empty() || label.len() > 63 {
            return false;
        }
        let bytes = label.as_bytes();
        if !bytes[0].is_ascii_alphanumeric() || !bytes[bytes.len() - 1].is_ascii_alphanumeric() {
            return false;
        }
        if !bytes
            .iter()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || *byte == b'-')
        {
            return false;
        }
    }
    true
}

fn placeholder_page(site: &Site) -> String {
    let domain = html_escape(&site.domain);
    let root = html_escape(&site.root_dir);
    format!(
        "<!doctype html>
<html lang=\"zh-CN\">
<head>
  <meta charset=\"utf-8\">
  <title>{domain}</title>
  <style>
    body {{ margin: 0; min-height: 100vh; display: flex; align-items: center;
           justify-content: center; background: #F7F4EC; color: #262040;
           font-family: 'MiSans', 'Microsoft YaHei', sans-serif; }}
    .card {{ padding: 36px 44px; background: #FFFFFF; border-radius: 16px;
            box-shadow: 0 12px 32px rgba(38, 32, 64, 0.08); text-align: center; }}
    h1 {{ margin: 0 0 8px; font-size: 20px; font-weight: 600; }}
    p {{ margin: 6px 0 0; font-size: 13px; color: #6F6985; }}
    code {{ color: #6C5CE7; }}
  </style>
</head>
<body>
  <div class=\"card\">
    <h1>{domain}</h1>
    <p>站点已经部署到 <code>{root}</code>。</p>
    <p>把这段占位页换成你自己的 <code>index.html</code> 就行。</p>
  </div>
</body>
</html>
"
    )
}

fn html_escape(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn text(map: &Map<String, Value>, key: &str) -> String {
    map.get(key).and_then(Value::as_str).unwrap_or("").to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_shapes() {
        assert!(looks_like_host("localhost"));
        assert!(looks_like_host("www.example.com"));
        assert!(looks_like_host("a-b.c-d.example"));
        assert!(!looks_like_host("http://example.com"));
        assert!(!looks_like_host("example.com:8080"));
        assert!(!looks_like_host("-bad.example"));
        assert!(!looks_like_host("bad-.example"));
        assert!(!looks_like_host(""));
    }

    #[test]
    fn validation_rejects_duplicates() {
        let existing = vec![Site {
            domain: "a.test".into(),
            aliases: vec!["alias.test".into()],
            port: 80,
            root_dir: "WWW/a".into(),
            ..Default::default()
        }];
        let clash = Site {
            domain: "a.test".into(),
            port: 81,
            root_dir: "WWW/x".into(),
            ..Default::default()
        };
        assert!(validate(&clash, None, &existing).is_err());
        assert!(validate(&clash, Some("a.test"), &existing).is_ok());
        let alias_clash = Site {
            domain: "b.test".into(),
            aliases: vec!["alias.test".into()],
            port: 81,
            root_dir: "WWW/x".into(),
            ..Default::default()
        };
        assert!(validate(&alias_clash, None, &existing).is_err());
    }
}
