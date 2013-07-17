#!/usr/bin/env ruby

class InvalidParseError < StandardError; end

class Session
  class << self
    def on_tag(tag, &cb)
      tag_handlers[tag] = cb
    end

    def on_tag_save(tag, field)
      on_tag tag do |data|
        @session_data[field.to_s] = data
      end
    end

    def tag_handlers
      @_tag_handlers ||= {}
    end
  end

  def tag_handlers
    self.class.tag_handlers
  end

  def initialize
    @session_data = {}
    @raw_data = []
    @closed = false
  end

  def closed?
    @closed
  end

  def data
    @session_data
  end

  on_tag :RxHeader do |data|
    md = /^\s*([^:]+):\s*(.*)$/.match(data)
    raise InvalidParseError, data unless md
    header_name, header_value = md[1], md[2]
    headers = (@session_data[:headers] ||= {})
    headers[header_name] = header_value
  end

  def add_data(tag, payload)
    @raw_data << "#{tag} #{payload}"
    instance_exec(payload, &tag_handlers[tag]) if tag_handlers.has_key?(tag)
  end

  on_tag :SessionOpen do |data|
    md = /^((?:\d+\.){3}\d+) \d+ (?:(?:\d+\.){3}\d+)?:\d+$/.match(data)
    raise InvalidParseError, data unless md
    @session_data['ip'] = md[1]
  end

  on_tag :SessionClose do |*|
    @closed = true
  end

  on_tag_save :RxProtocol, :protocol
  on_tag_save :RxURL, :url
  on_tag_save :RxRequest, :method

  on_tag_save :TxStatus, :status
end

$sessions = {}
$finished_sessions = []

ARGF.each do |line|
  md = /^\s*(\d+)\s+([^\s]+)\s+c\s+(.+)$/.match(line)
  raise InvalidParseError, line unless md
  session_id, tag, payload = md[1].to_i, md[2].to_sym, md[3]
  $sessions[session_id] = Session.new unless $sessions.has_key?(session_id)
  session = $sessions[session_id]
  session.add_data(tag, payload)
  if session.closed?
    $finished_sessions << session
    $sessions.delete(session_id)
    puts session.data
    puts session.instance_variable_get(:@raw_data).join("\n")
  end
end
