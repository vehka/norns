--- bluetooth midi
--
-- wrapper over `bluetoothctl` and `aconnect`. a connected BLE MIDI device
-- shows up as an ALSA sequencer port owned by bluetoothd; a midi device
-- connected to that port is added for it, so it is listed by name in
-- SYSTEM > DEVICES > MIDI.
--
-- @module bluetooth

local util = require "util"

local Bluetooth = {}

local SCAN_SECONDS = 8
-- bluetoothctl waits forever if bluetoothd does not answer
local CTL = "timeout 5 bluetoothctl "
local BLUETOOTH_SUFFIX = " Bluetooth"
local WIRE_SECONDS = 5

-- 33 is one of the metros reserved for the system
local wire_timer = metro[33]

Bluetooth.status = "unavailable"
-- devices known to bluetoothd: list of {addr=, name=}
Bluetooth.devices = {}
-- names of devices that currently have a midi port
Bluetooth.connected = {}

-- true while a scan or connect is running; they set the status themselves
local busy = false
-- true while the background wiring timer runs
local wiring = false
-- midi devices added so far: sequencer address -> {name=, idle=}
local wired = {}

--
-- common functions
--

local function parse_devices(output)
  local devices = {}
  for addr, name in output:gmatch("Device (%x%x:%x%x:%x%x:%x%x:%x%x:%x%x) ([^\n]+)") do
    -- unnamed devices are listed with their address as the name
    if name:gsub("-", ":") ~= addr then
      table.insert(devices, {addr = addr, name = name})
    end
  end
  return devices
end

-- parse `aconnect -l` into the list of bluetooth midi ports
local function parse_ports(output)
  local ports = {}
  local client = nil
  local port = nil
  for line in output:gmatch("[^\n]+") do
    local c = line:match("^client (%d+):")
    if c then
      client = c
      port = nil
    elseif line:match("^%s+Connecting To: ") or line:match("^%s+Connected From: ") then
      if port then port.idle = false end
    else
      local p, name = line:match("^%s+(%d+) '(.-)%s*'")
      port = nil
      if p and client and name:sub(-#BLUETOOTH_SUFFIX) == BLUETOOTH_SUFFIX then
        port = {
          id = client .. ":" .. p,
          name = name:sub(1, -#BLUETOOTH_SUFFIX - 1),
          idle = true
        }
        table.insert(ports, port)
      end
    end
  end
  return ports
end

local function find(name)
  for _, d in ipairs(Bluetooth.devices) do
    if d.name == name then return d end
  end
  return nil
end

local function names(list)
  local t = {}
  for _, d in ipairs(list) do table.insert(t, d.name) end
  return t
end

--
-- module functions
--

--- true if there is a bluetooth controller.
function Bluetooth.available()
  return util.os_capture("ls /sys/class/bluetooth 2>/dev/null") ~= ""
end

--- add a midi device for every bluetooth midi port, and remove the
-- devices whose port is gone.
-- @tparam ?string output output of `aconnect -l`, fetched if omitted
function Bluetooth.wire(output)
  output = output or util.os_capture("aconnect -l", true)
  local ports = parse_ports(output)
  Bluetooth.connected = names(ports)
  local seen = {}
  for _, port in ipairs(ports) do
    local w = wired[port.id]
    -- a port that reappears at the same address has lost its connections.
    -- wait for a second look: this listing may predate the connection
    if w and (w.name ~= port.name or (port.idle and w.idle)) then
      _norns.midi_seq_disconnect(port.id)
      w = nil
    end
    if w == nil then
      _norns.midi_seq_connect(port.name, port.id)
      wired[port.id] = {name = port.name, idle = false}
    else
      w.idle = port.idle
    end
    seen[port.id] = true
  end
  for id, _ in pairs(wired) do
    if not seen[id] then
      _norns.midi_seq_disconnect(id)
      wired[id] = nil
    end
  end
end

-- wire devices in the background, so that a device that reconnects by
-- itself gets wired without the menu page being open.
local function start_wiring()
  if wiring then return end
  wiring = true
  wire_timer.time = WIRE_SECONDS
  wire_timer.count = -1
  wire_timer.event = function()
    norns.system_cmd("aconnect -l", Bluetooth.wire)
  end
  wire_timer:start()
end

local function stop_wiring()
  if not wiring then return end
  wiring = false
  wire_timer:stop()
end

--- start background wiring if bluetoothd already knows a device.
-- nothing runs until a device has been connected.
function Bluetooth.init()
  if not Bluetooth.available() then return end
  norns.system_cmd(CTL .. "devices", function(output)
    if #parse_devices(output) > 0 then start_wiring() end
  end)
end

-- take in the output of `bluetoothctl devices` and `aconnect -l`.
-- empty output means there is no controller
local function refresh(output)
  if output == "" then
    Bluetooth.status = "unavailable"
    Bluetooth.devices = {}
    Bluetooth.connected = {}
    return
  end
  Bluetooth.devices = parse_devices(output)
  Bluetooth.wire(output)
  if not busy then
    Bluetooth.status = #Bluetooth.connected > 0 and "connected" or "ready"
  end
end

-- run a command, then refresh from the listings it leaves behind
local function run(cmd, callback)
  cmd = "[ -n \"$(ls /sys/class/bluetooth 2>/dev/null)\" ] && { "
    .. (cmd or "") .. CTL .. "devices; aconnect -l; }"
  norns.system_cmd(cmd, function(output)
    if callback then callback(output) else refresh(output) end
  end)
end

--- refresh the device list and the connected midi ports.
-- @tparam ?func callback called when done
function Bluetooth.update(callback)
  run(nil, function(output)
    refresh(output)
    if callback then callback() end
  end)
end

--- power the controller on.
function Bluetooth.on()
  norns.system_cmd(CTL .. "power on", function() end)
end

--- scan for devices.
-- @tparam ?func callback called with the list of device names when done
function Bluetooth.scan(callback)
  if busy then return end
  Bluetooth.status = "scanning..."
  busy = true
  local cmd = "bluetoothctl --timeout " .. SCAN_SECONDS .. " scan on >/dev/null; "
  run(cmd, function(output)
    busy = false
    refresh(output)
    if callback then callback(Bluetooth.device_names()) end
  end)
end

--- connect a device and add its midi device.
-- @tparam string name device name, as listed by device_names
-- @tparam ?func callback called with true or false when done
function Bluetooth.connect(name, callback)
  local d = find(name)
  if d == nil or busy then return end
  Bluetooth.status = "connecting..."
  busy = true
  -- a device found by an earlier scan may have expired; scan once and retry.
  -- the midi port appears shortly after the connection.
  local connect = "timeout 30 bluetoothctl connect " .. d.addr
  local cmd = "o=$(" .. connect .. "); case \"$o\" in *successful*) ;; *)"
    .. " bluetoothctl --timeout " .. SCAN_SECONDS .. " scan on >/dev/null;"
    .. " o=$(" .. connect .. ");; esac; echo \"$o\"; sleep 2; "
  run(cmd, function(output)
    local ok = output:find("Connection successful", 1, true) ~= nil
    busy = false
    refresh(output)
    if ok then
      start_wiring()
    else
      Bluetooth.status = "failed"
    end
    if callback then callback(ok) end
  end)
end

--- disconnect a device.
-- @tparam string name device name
-- @tparam ?func callback called when done
function Bluetooth.disconnect(name, callback)
  local d = find(name)
  if d == nil then return end
  run(CTL .. "disconnect " .. d.addr .. " >/dev/null; ", function(output)
    refresh(output)
    if callback then callback() end
  end)
end

--- forget a device.
-- @tparam string name device name
-- @tparam ?func callback called when done
function Bluetooth.forget(name, callback)
  local d = find(name)
  if d == nil then return end
  -- disconnect first: removing a connected device leaves it on the
  -- controller's auto-connect list, and it comes back by itself
  local cmd = CTL .. "disconnect " .. d.addr .. " >/dev/null; "
    .. CTL .. "remove " .. d.addr .. " >/dev/null; "
  run(cmd, function(output)
    refresh(output)
    if #Bluetooth.devices == 0 then stop_wiring() end
    if callback then callback() end
  end)
end

--- names of the known devices.
function Bluetooth.device_names()
  return names(Bluetooth.devices)
end

return Bluetooth
