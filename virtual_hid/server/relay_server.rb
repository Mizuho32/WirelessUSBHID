#!/usr/bin/env ruby
# frozen_string_literal: true
#
# UDP -> WebSocket の薄い中継サーバー。
#
# ローカルネットワーク上の機器(ESP32-S3 host role や、独自の入力キャプチャ
# スクリプトなど)から届く16byteバイナリHIDパケットを、そのまま
# WebSocket接続中の全クライアントへブロードキャストするだけ。
# パケットの中身(mouse/keyboard/consumer)は一切解釈しない -
# 解釈はクライアント(virtual_hid/client)側の役目。
#
# 使い方:
#   ruby relay_server.rb [--udp-port 4210] [--ws-port 8765] [--token SECRET]
#
# セキュア化(TLS)はリバースプロキシ(nginx/caddy等)に任せる前提。
# このプロセス自体はローカル(プロキシの裏)で平文 ws:// のみ待ち受ける。

require "em-websocket"
require "eventmachine"
require "socket"
require "optparse"
require "securerandom"

PACKET_MAGIC = 0xCAFE
MIN_PACKET_SIZE = 16
MAX_PACKET_SIZE = 16

options = {
  udp_host: "0.0.0.0",
  udp_port: 4210,
  ws_host: "0.0.0.0",
  ws_port: 8765,
  token: ENV["VHID_TOKEN"],
}

OptionParser.new do |opts|
  opts.banner = "Usage: relay_server.rb [options]"
  opts.on("--udp-host HOST", "UDP受信アドレス (default: 0.0.0.0)") { |v| options[:udp_host] = v }
  opts.on("--udp-port PORT", Integer, "UDP受信ポート (default: 4210)") { |v| options[:udp_port] = v }
  opts.on("--ws-host HOST", "WebSocket待受アドレス (default: 0.0.0.0)") { |v| options[:ws_host] = v }
  opts.on("--ws-port PORT", Integer, "WebSocket待受ポート (default: 8765)") { |v| options[:ws_port] = v }
  opts.on("--token TOKEN", "クライアント認証トークン (未指定ならVHID_TOKEN環境変数、それも無ければ無認証)") { |v| options[:token] = v }
end.parse!

if options[:token].nil? || options[:token].empty?
  warn "[WARN] トークン未設定。--token か環境変数 VHID_TOKEN で設定することを強く推奨(グローバル公開時は必須)。"
end

# --- UDP側: 受信したバイトを検証してWS接続中の全クライアントへ配る -------
class UdpRelay < EM::Connection
  def initialize(clients)
    super()
    @clients = clients
  end

  def receive_data(data)
    puts("[UDP] received #{data}")
    #puts("[UDP] received #{data.unpack("B*")}")
    return unless valid_packet?(data)

    @clients.each do |ws|
      #puts("[UDP] relaied #{data}")
      ws.send_binary(data)
    rescue StandardError => e
      warn "[WS] send failed: #{e.message}"
    end
  end

  private

  # 16byte固定・先頭2byteが magic (0xCAFE, リトルエンディアン) であることだけ確認する。
  # 中身の意味(mouse/keyboard/consumer)は関知しない。
  def valid_packet?(data)
    return false unless data.bytesize.between?(MIN_PACKET_SIZE, MAX_PACKET_SIZE)

    magic = data[0, 2].unpack1("v")
    magic == PACKET_MAGIC
  end
end

clients = []

EM.run do
  EM.open_datagram_socket(options[:udp_host], options[:udp_port], UdpRelay, clients)
  puts "[UDP] listening on #{options[:udp_host]}:#{options[:udp_port]}"

  EM::WebSocket.run(host: options[:ws_host], port: options[:ws_port]) do |ws|
    ws.onopen do |handshake|
      if options[:token] && !options[:token].empty?
        supplied = handshake.query["token"]
        if supplied != options[:token]
          warn "[WS] rejected connection from #{ws.remote_ip rescue "?"}: bad token"
          ws.close(4001, "unauthorized")
          next
        end
      end

      clients << ws
      puts "[WS] client connected (#{clients.size} total)"
    end

    ws.onclose do
      clients.delete(ws)
      puts "[WS] client disconnected (#{clients.size} total)"
    end

    ws.onerror do |e|
      warn "[WS] error: #{e.message}"
    end
  end
  puts "[WS] listening on #{options[:ws_host]}:#{options[:ws_port]} (path: /, auth: #{options[:token] ? "token" : "none"})"
end
