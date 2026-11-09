from pathlib import Path

root = Path(__file__).resolve().parents[1]
web = (root / "main/web_server.c").read_text(encoding="utf-8")
page = (root / "main/web/index.html").read_text(encoding="utf-8")
settings = (root / "main/settings.c").read_text(encoding="utf-8")
password = (root / "main/auth_password.c").read_text(encoding="utf-8")
wifi = (root / "main/wifi_manager.c").read_text(encoding="utf-8")
config = (root / "main/app_config.h").read_text(encoding="utf-8")

assert 'lightgw_session=' in web
assert 'SameSite=Strict' in web and 'HttpOnly' in web
assert '/api/auth/info' in web and '/api/auth/login' in web
assert 'WWW-Authenticate' not in web
assert 'settings_verify_admin(user_copy, password_copy)' in web
assert 'settings_upgrade_admin_hash_async' in web
assert 'Administrator login required' in web
assert 'id="loginPassword" type="password"' in page
assert 'loginPromise' in page and "r.status===401" in page
assert 'APP_AUTH_LEGACY_ITERATIONS    120000' in config
assert 'APP_AUTH_ITERATIONS           8192' in config
assert 'vTaskDelay(1)' in password
assert 'yield_interval' in password
assert 'auth_iter' in settings
assert 'Administrator hash migrated' in settings
assert 'APP_PROVISION_AP_PASSWORD     "admin123"' in config
assert 'strlcpy(s_ap_password, APP_PROVISION_AP_PASSWORD' in wifi
assert "esp_random" not in wifi
assert '"/api/settings/device-name"' in web
assert '"/api/settings/admin-password"' in web
assert "settings_save_device_name(name_copy)" in web
assert "settings_verify_admin(username_copy, current_copy)" in web
assert "settings_save_admin(username_copy, replacement_copy)" in web
assert 'mdns_updates_after_restart' in web
assert 'sessions_cleared' in web
assert 'id="runtimeDeviceName"' in page
assert 'id="currentAdminPassword" type="password"' in page
assert "api('/api/settings/device-name'" in page
assert "api('/api/settings/admin-password'" in page
for tab in ("operation", "schedules", "status", "backup", "time", "extensions", "rainmaker", "logs"):
    assert f'data-tab="{tab}"' in page
assert 'class="active" data-tab="operation"' in page
assert "function showTab(name)" in page

print("AUTH_CONTRACT_TEST PASS")
