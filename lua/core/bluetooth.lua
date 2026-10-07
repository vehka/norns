--- bluetooth midi
--
-- wrapper over `bluetoothctl` and `aconnect`. a connected BLE MIDI device
-- shows up as an ALSA sequencer port owned by bluetoothd; that port is
-- wired to matron's "virtual" midi device, so all bluetooth devices share
-- the `virtual` entry in SYSTEM > DEVICES > MIDI.
--
-- @module bluetooth

local util = require "util"

local Bluetooth = {}

local SCAN_SECONDS = 8
local VIRTUAL_PORT = "Virtual RawMIDI"
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

-- parse `aconnect -l` into the virtual port and the bluetooth midi ports
local function parse_ports(output)
  local virtual = nil
  local ports = {}
  local client = nil
  local port = nil
  for line in output:gmatch("[^\n]+") do
    local c = line:match("^client (%d+):")
    local to = line:match("^%s+Connecting To: (.+)")
    local from = line:match("^%s+Connected From: (.+)")
    if c then
      client = c
      port = nil
    elseif to and port then
      port.to = to
    elseif from and port then
      port.from = from
    else
      local p, name = line:match("^%s+(%d+) '(.-)%s*'")
      port = nil
      if p and client then
        if name == VIRTUAL_PORT then
          virtual = client .. ":" .. p
        elseif name:sub(-#BLUETOOTH_SUFFIX) == BLUETOOTH_SUFFIX then
          port = {
            id = client .. ":" .. p,
            name = name:sub(1, -#BLUETOOTH_SUFFIX - 1),
            to = "",
            from = ""
          }
          table.insert(ports, port)
        end
      end
    end
  end
  return virtual, ports
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

--- wire every bluetooth midi port to the virtual midi device, both ways.
-- existing connections are left alone.
-- @tparam ?string output output of `aconnect -l`, fetched if omitted
function Bluetooth.wire(output)
  output = output or util.os_capture("aconnect -l", true)
  local virtual, ports = parse_ports(output)
  Bluetooth.connected = names(ports)
  if virtual == nil then return end
  for _, port in ipairs(ports) do
    if not port.to:find(virtual, 1, true) then
      _norns.execute("aconnect " .. port.id .. " " .. virtual .. " 2>/dev/null")
    end
    if not port.from:find(virtual, 1, true) then
      _norns.execute("aconnect " .. virtual .. " " .. port.id .. " 2>/dev/null")
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
  norns.system_cmd("timeout 5 bluetoothctl devices", function(output)
    if #parse_devices(output) > 0 then start_wiring() end
  end)
end

--- refresh the device list and the connected midi ports.
function Bluetooth.update()
  if not Bluetooth.available() then
    Bluetooth.status = "unavailable"
    Bluetooth.devices = {}
    Bluetooth.connected = {}
    return
  end
  Bluetooth.devices = parse_devices(util.os_capture("bluetoothctl devices", true))
  Bluetooth.wire()
  if not busy then
    Bluetooth.status = #Bluetooth.connected > 0 and "connected" or "ready"
  end
end

--- power the controller on.
function Bluetooth.on()
  _norns.execute("bluetoothctl power on")
end

--- scan for devices.
-- @tparam ?func callback called with the list of device names when done
function Bluetooth.scan(callback)
  Bluetooth.status = "scanning..."
  busy = true
  local cmd = "bluetoothctl --timeout " .. SCAN_SECONDS .. " scan on >/dev/null;"
    .. " bluetoothctl devices"
  norns.system_cmd(cmd, function(output)
    Bluetooth.devices = parse_devices(output)
    busy = false
    Bluetooth.update()
    if callback then callback(Bluetooth.device_names()) end
  end)
end

--- connect a device and wire its midi port.
-- @tparam string name device name, as listed by device_names
-- @tparam ?func callback called with true or false when done
function Bluetooth.connect(name, callback)
  local d = find(name)
  if d == nil then return end
  Bluetooth.status = "connecting..."
  busy = true
  -- a device found by an earlier scan may have expired; scan once and retry.
  -- the midi port appears shortly after the connection.
  local connect = "bluetoothctl connect " .. d.addr
  local cmd = "o=$(" .. connect .. "); case \"$o\" in *successful*) ;; *)"
    .. " bluetoothctl --timeout " .. SCAN_SECONDS .. " scan on >/dev/null;"
    .. " o=$(" .. connect .. ");; esac; echo \"$o\"; sleep 2; aconnect -l"
  norns.system_cmd(cmd, function(output)
    local ok = output:find("Connection successful", 1, true) ~= nil
    busy = false
    Bluetooth.update()
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
function Bluetooth.disconnect(name)
  local d = find(name)
  if d == nil then return end
  _norns.execute("bluetoothctl disconnect " .. d.addr)
  Bluetooth.update()
end

--- forget a device.
-- @tparam string name device name
function Bluetooth.forget(name)
  local d = find(name)
  if d == nil then return end
  -- disconnect first: removing a connected device leaves it on the
  -- controller's auto-connect list, and it comes back by itself
  _norns.execute("bluetoothctl disconnect " .. d.addr)
  _norns.execute("bluetoothctl remove " .. d.addr)
  Bluetooth.update()
  if #Bluetooth.devices == 0 then stop_wiring() end
end

--- names of the known devices.
function Bluetooth.device_names()
  return names(Bluetooth.devices)
end

return Bluetooth
