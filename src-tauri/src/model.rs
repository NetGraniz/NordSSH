use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use zeroize::{Zeroize, ZeroizeOnDrop};

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Host {
    pub id: String,
    pub name: String,
    pub hostname: String,
    pub port: u16,
    pub username: String,
    pub vault: String,
    pub auth: String,
    pub key_path: String,
    pub certificate_path: String,
    pub favorite: bool,
}
impl Host {
    pub fn validate(&self) -> Result<(), String> {
        if uuid::Uuid::parse_str(&self.id).is_err() {
            return Err("Invalid host identifier".into());
        }
        for value in [&self.name, &self.hostname, &self.username, &self.vault] {
            if value.is_empty() || value.len() > 256 || value.chars().any(char::is_control) {
                return Err("Host fields must contain 1–256 printable characters".into());
            }
        }
        if self.port == 0 || !["password", "key", "certificate"].contains(&self.auth.as_str()) {
            return Err("Invalid port or authentication method".into());
        }
        for path in [&self.key_path, &self.certificate_path] {
            if path.len() > 4096 || path.contains('\0') {
                return Err("Invalid key path".into());
            }
        }
        Ok(())
    }
    pub fn endpoint(&self) -> String {
        format!("[{}]:{}", self.hostname.to_ascii_lowercase(), self.port)
    }
    pub fn same_login_as(&self, other: &Self) -> bool {
        self.endpoint() == other.endpoint()
            && self.username == other.username
            && self.auth == other.auth
            && self.key_path == other.key_path
            && self.certificate_path == other.certificate_path
    }
}
#[derive(Clone, Default, Serialize, Deserialize, Zeroize, ZeroizeOnDrop)]
#[serde(deny_unknown_fields)]
pub struct Secret {
    pub password: String,
    pub passphrase: String,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Settings {
    pub theme: String,
    pub font_size: u16,
    pub check_updates: bool,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            theme: "nord".into(),
            font_size: 14,
            check_updates: true,
        }
    }
}
impl Settings {
    pub fn validate(&self) -> Result<(), String> {
        if !["nord", "midnight"].contains(&self.theme.as_str())
            || !(11..=24).contains(&self.font_size)
        {
            return Err("Invalid appearance setting".into());
        }
        Ok(())
    }
}
#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Snippet {
    pub id: String,
    pub name: String,
    pub command: String,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Vault {
    pub schema: u32,
    pub hosts: Vec<Host>,
    pub secrets: BTreeMap<String, Secret>,
    pub known_hosts: BTreeMap<String, String>,
    pub settings: Settings,
    pub snippets: Vec<Snippet>,
}
impl Default for Vault {
    fn default() -> Self {
        Self {
            schema: 1,
            hosts: vec![],
            secrets: BTreeMap::new(),
            known_hosts: BTreeMap::new(),
            settings: Settings::default(),
            snippets: vec![],
        }
    }
}
impl Vault {
    pub fn validate(&self) -> Result<(), String> {
        if self.schema != 1 {
            return Err("Unsupported vault schema; no data was changed".into());
        }
        if self.hosts.len() > 2000 || self.snippets.len() > 1000 || self.known_hosts.len() > 4000 {
            return Err("Vault exceeds supported limits".into());
        }
        let mut ids = std::collections::HashSet::new();
        for h in &self.hosts {
            h.validate()?;
            if !ids.insert(&h.id) {
                return Err("Duplicate host identifier".into());
            }
        }
        for s in &self.snippets {
            if uuid::Uuid::parse_str(&s.id).is_err()
                || s.name.is_empty()
                || s.name.len() > 256
                || s.command.len() > 16384
            {
                return Err("Invalid snippet".into());
            }
        }
        self.settings.validate()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn future_schema_is_rejected() {
        let v = Vault {
            schema: 2,
            ..Vault::default()
        };
        assert!(v.validate().is_err());
    }
    #[test]
    fn invalid_settings_are_rejected() {
        let s = Settings {
            font_size: 65535,
            ..Settings::default()
        };
        assert!(s.validate().is_err());
    }
    #[test]
    fn empty_vault_is_valid() {
        assert!(Vault::default().validate().is_ok());
    }
    #[test]
    fn saved_credentials_do_not_follow_a_changed_login() {
        let host = Host {
            id: uuid::Uuid::new_v4().to_string(),
            name: "test".into(),
            hostname: "localhost".into(),
            port: 22,
            username: "test".into(),
            vault: "Local".into(),
            auth: "password".into(),
            key_path: String::new(),
            certificate_path: String::new(),
            favorite: false,
        };
        let mut renamed = host.clone();
        renamed.name = "new display name".into();
        assert!(host.same_login_as(&renamed));
        renamed.hostname = "other-host".into();
        assert!(!host.same_login_as(&renamed));
        renamed = host.clone();
        renamed.username = "other-user".into();
        assert!(!host.same_login_as(&renamed));
    }
    #[test]
    fn duplicate_hosts_are_rejected() {
        let h = Host {
            id: uuid::Uuid::new_v4().to_string(),
            name: "test".into(),
            hostname: "localhost".into(),
            port: 22,
            username: "test".into(),
            vault: "Local".into(),
            auth: "password".into(),
            key_path: String::new(),
            certificate_path: String::new(),
            favorite: false,
        };
        let v = Vault {
            hosts: vec![h.clone(), h],
            ..Vault::default()
        };
        assert!(v.validate().is_err());
    }
}
