#!/usr/bin/env python3
"""
Discord HTTPS Proxy for RadiumOS
Forwards HTTP requests from RadiumOS to Discord API over HTTPS
"""
import socket
import ssl
import sys
from datetime import datetime

def main():
    print("="*60)
    print("       DISCORD HTTPS PROXY")
    print("="*60)
    print()
    print("Starting proxy on 0.0.0.0:8080...")
    
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server.bind(('0.0.0.0', 8080))
        server.listen(5)
        print("\033[32m[SUCCESS]\033[0m Proxy is listening on port 8080")
        print()
        print("RadiumOS should connect to: 10.0.2.2:8080")
        print("Waiting for requests... (Press Ctrl+C to stop)")
        print("-"*60)
        print()
        
        while True:
            client, address = server.accept()
            client.settimeout(10.0)  # 10 second timeout
            print(f"\033[1;32m[{datetime.now().strftime('%H:%M:%S')}]\033[0m Connection from {address[0]}:{address[1]}")
            
            try:
                # Receive HTTP request from RadiumOS
                request_data = b''
                while True:
                    try:
                        chunk = client.recv(4096)
                        if not chunk:
                            break
                        request_data += chunk
                        # If we have headers and body (or no body expected), stop
                        if b'\r\n\r\n' in request_data:
                            # Check if there's a Content-Length
                            headers_end = request_data.find(b'\r\n\r\n')
                            headers = request_data[:headers_end].decode('utf-8', errors='ignore')
                            
                            content_length = 0
                            for line in headers.split('\r\n'):
                                if line.lower().startswith('content-length:'):
                                    content_length = int(line.split(':')[1].strip())
                            
                            # Check if we have all the body
                            body_received = len(request_data) - (headers_end + 4)
                            if body_received >= content_length:
                                break
                    except socket.timeout:
                        break
                
                if not request_data:
                    client.close()
                    continue
                
                request_text = request_data.decode('utf-8', errors='ignore')
                print(f"  Received {len(request_data)} bytes")
                
                # Parse the request
                lines = request_text.split('\r\n')
                if len(lines) < 1:
                    client.close()
                    continue
                
                request_line = lines[0]
                print(f"  Request: {request_line}")
                
                # Extract method, path, and headers
                parts = request_line.split(' ')
                if len(parts) < 2:
                    client.close()
                    continue
                
                method = parts[0]
                path = parts[1]
                
                # Extract headers
                headers = {}
                body_start = None
                for i, line in enumerate(lines[1:], 1):
                    if line == '':
                        body_start = i + 1
                        break
                    if ':' in line:
                        key, value = line.split(':', 1)
                        headers[key.strip().lower()] = value.strip()
                
                # Extract body if present
                body = ''
                if body_start and body_start < len(lines):
                    body = '\r\n'.join(lines[body_start:])
                
                print(f"  Method: {method}")
                print(f"  Path: {path}")
                print(f"  Headers: {len(headers)} headers")
                
                # Forward to Discord API over HTTPS
                print(f"  \033[33m[FORWARDING]\033[0m to discord.com{path}")
                
                # Create HTTPS connection to Discord with timeout
                context = ssl.create_default_context()
                discord_sock = socket.create_connection(('discord.com', 443), timeout=10)
                discord_sock.settimeout(10.0)
                discord_ssl = context.wrap_socket(discord_sock, server_hostname='discord.com')
                
                # Build request to Discord
                discord_request = f"{method} {path} HTTP/1.1\r\n"
                discord_request += "Host: discord.com\r\n"
                
                # Forward important headers
                if 'authorization' in headers:
                    discord_request += f"Authorization: {headers['authorization']}\r\n"
                if 'content-type' in headers:
                    discord_request += f"Content-Type: {headers['content-type']}\r\n"
                if 'content-length' in headers:
                    discord_request += f"Content-Length: {headers['content-length']}\r\n"
                
                discord_request += "User-Agent: RadiumOS/1.0\r\n"
                discord_request += "Connection: close\r\n"
                discord_request += "\r\n"
                
                if body:
                    discord_request += body
                
                # Send to Discord
                print(f"  Sending {len(discord_request)} bytes to Discord...")
                discord_ssl.sendall(discord_request.encode('utf-8'))
                
                # Receive response from Discord with timeout
                print(f"  Waiting for Discord response...")
                discord_response = b''
                try:
                    while True:
                        chunk = discord_ssl.recv(4096)
                        if not chunk:
                            print(f"  Discord closed connection")
                            break
                        discord_response += chunk
                        print(f"  Received {len(chunk)} bytes (total: {len(discord_response)})")
                        
                        # Check if we have complete response
                        if b'\r\n\r\n' in discord_response:
                            # Parse Content-Length
                            headers_part = discord_response.split(b'\r\n\r\n')[0]
                            headers_text = headers_part.decode('utf-8', errors='ignore')
                            
                            content_length = None
                            chunked = False
                            
                            for line in headers_text.split('\r\n'):
                                if line.lower().startswith('content-length:'):
                                    content_length = int(line.split(':')[1].strip())
                                elif line.lower().startswith('transfer-encoding:') and 'chunked' in line.lower():
                                    chunked = True
                            
                            if not chunked and content_length is not None:
                                body_start = discord_response.find(b'\r\n\r\n') + 4
                                body_received = len(discord_response) - body_start
                                if body_received >= content_length:
                                    print(f"  Complete response received")
                                    break
                except socket.timeout:
                    print(f"  Timeout while receiving from Discord")
                
                discord_ssl.close()
                
                print(f"  \033[32m[RECEIVED]\033[0m {len(discord_response)} bytes from Discord")
                
                if len(discord_response) == 0:
                    error_response = (
                        b"HTTP/1.1 502 Bad Gateway\r\n"
                        b"Content-Type: text/plain\r\n"
                        b"Content-Length: 28\r\n"
                        b"\r\n"
                        b"No response from Discord API"
                    )
                    client.sendall(error_response)
                    client.close()
                    continue
                
                # Show response preview
                response_text = discord_response.decode('utf-8', errors='ignore')
                status_line = response_text.split('\r\n')[0] if '\r\n' in response_text else 'Unknown'
                print(f"  Status: {status_line}")
                
                # Show JSON preview if present
                if '{"' in response_text or '[{' in response_text:
                    json_start = response_text.find('{')
                    if json_start == -1:
                        json_start = response_text.find('[')
                    if json_start != -1:
                        json_preview = response_text[json_start:json_start+100]
                        print(f"  JSON preview: {json_preview}...")
                
                # Forward response back to RadiumOS
                print(f"  Sending {len(discord_response)} bytes back to RadiumOS...")
                client.sendall(discord_response)
                print(f"  \033[32m[FORWARDED]\033[0m Response sent back to RadiumOS")
                print()
                
            except socket.timeout:
                print(f"  \033[31m[TIMEOUT]\033[0m Connection timed out")
                error_response = (
                    b"HTTP/1.1 504 Gateway Timeout\r\n"
                    b"Content-Type: text/plain\r\n"
                    b"Content-Length: 19\r\n"
                    b"\r\n"
                    b"Connection timeout\r\n"
                )
                try:
                    client.sendall(error_response)
                except:
                    pass
            except Exception as e:
                print(f"  \033[31m[ERROR]\033[0m {e}")
                import traceback
                traceback.print_exc()
                # Send error response
                error_msg = str(e).encode('utf-8')[:100]
                error_response = (
                    b"HTTP/1.1 502 Bad Gateway\r\n"
                    b"Content-Type: text/plain\r\n"
                    b"Content-Length: " + str(len(error_msg)).encode() + b"\r\n"
                    b"\r\n" +
                    error_msg
                )
                try:
                    client.sendall(error_response)
                except:
                    pass
            finally:
                client.close()
                print(f"  Connection closed")
                
    except PermissionError:
        print("\033[31m[ERROR]\033[0m Permission denied!")
        print("Try: sudo python3 discord_proxy.py")
    except OSError as e:
        if "Address already in use" in str(e):
            print("\033[31m[ERROR]\033[0m Port 8080 is already in use!")
            print("Kill the other process: lsof -i :8080")
        else:
            print(f"\033[31m[ERROR]\033[0m {e}")
    except KeyboardInterrupt:
        print()
        print("\033[33m[INFO]\033[0m Shutting down proxy...")
    finally:
        server.close()
        print("\033[32m[INFO]\033[0m Proxy stopped")

if __name__ == '__main__':
    main()