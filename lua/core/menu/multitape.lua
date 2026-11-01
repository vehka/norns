-- menu/multitape.lua
-- 4TRK: a 4-track tape view in the spirit of the OP-1 tape.
-- the playhead is fixed in the centre and the tape scrolls past it.
--
-- E2 select track        K2 play / stop
-- E3 scrub               K3 record-arm selected track (punch in/out while playing)
-- K1+E2 track level      K1+K2 back to start
-- K1+E3 zoom             K1+K3 track / project actions

local fileselect = require 'fileselect'
local listselect = require 'listselect'
local textentry = require 'textentry'

local MT = audio.multitape

local PLAYHEAD_X = 64
local TRACK_Y = {26, 36, 46, 56}
local ZOOMS = {1/128, 1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1, 2, 3} -- seconds per pixel
local TICKS = {0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60}             -- seconds
local FPS = 20

local m = {
  track = 1,
  zoom = 4,
  status = nil,
  modal = false,
  msg = nil,
  msg_until = 0,
}

local function now() return util.time() end

local function flash(text, secs)
  m.msg = text
  m.msg_until = now() + (secs or 1.5)
end

local function format_time(s)
  local mins = math.floor(s / 60)
  local secs = s - mins * 60
  return string.format("%d:%04.1f", mins, secs)
end

local function refresh_status()
  m.status = MT.status()
end

-- record-arm is also the record switch: armed + playing = recording
local function set_armed(track)
  MT.arm(track)
  MT.rec(track > 0)
end

local function modal(fn)
  m.modal = true
  fn()
end

local function end_modal()
  m.modal = false
  refresh_status()
  _menu.redraw()
end

--- actions

local function load_file(at_playhead)
  local t = m.track
  modal(function()
    fileselect.enter(_path.audio, function(path)
      if path ~= "cancel" then
        if m.status.armed == t then set_armed(0) end
        MT.load(t, path, at_playhead and m.status.pos or 0)
        flash("LOADING")
      end
      end_modal()
    end, "audio")
  end)
end

local function save_project()
  modal(function()
    textentry.enter(function(name)
      if name and name ~= "" then
        MT.save(name)
        flash("SAVED " .. name)
      end
      end_modal()
    end, os.date("%y%m%d-%H%M"), "save 4trk as:")
  end)
end

local function open_project()
  util.make_dir(MT.dir())
  modal(function()
    fileselect.enter(MT.dir(), function(path)
      if path ~= "cancel" then
        local name = path:match("multitape/([^/]+)/[^/]*$")
        if name then
          set_armed(0)
          MT.open(name)
          flash("OPENING " .. name)
        else
          flash("NOT A 4TRK FOLDER")
        end
      end
      end_modal()
    end)
  end)
end

local function track_actions()
  local t = m.track
  local muted = params:get("mtape_mute_" .. t) == 1
  local options = {
    "load file at start",
    "load file at playhead",
    muted and "unmute track " .. t or "mute track " .. t,
    "clear track " .. t,
    "save 4trk",
    "open 4trk",
  }
  modal(function()
    listselect.enter(options, function(choice)
      local deferred = false
      if choice == options[1] then
        deferred = true
        load_file(false)
      elseif choice == options[2] then
        deferred = true
        load_file(true)
      elseif choice == options[3] then
        params:set("mtape_mute_" .. t, muted and 0 or 1)
      elseif choice == options[4] then
        if m.status.armed == t then set_armed(0) end
        MT.clear(t)
        flash("CLEARED " .. t)
      elseif choice == options[5] then
        deferred = true
        save_project()
      elseif choice == options[6] then
        deferred = true
        open_project()
      end
      if not deferred then end_modal() end
    end)
  end)
end

--- input

m.key = function(n, z)
  if z ~= 1 or not m.status then return end
  local s = m.status
  if n == 2 then
    if _menu.alt then
      MT.seek(0)
    elseif s.playing then
      MT.stop()
    else
      MT.play()
    end
  elseif n == 3 then
    if _menu.alt then
      track_actions()
    elseif s.armed == m.track then
      set_armed(0)
    else
      set_armed(m.track)
    end
  end
  refresh_status()
  _menu.redraw()
end

m.enc = function(n, d)
  if not m.status then return end
  if n == 2 then
    if _menu.alt then
      params:delta("mtape_level_" .. m.track, d)
      flash(string.format("TRACK %d %s", m.track, params:string("mtape_level_" .. m.track)), 1)
    else
      m.track = util.clamp(m.track + d, 1, MT.TRACKS)
    end
  elseif n == 3 then
    if _menu.alt then
      m.zoom = util.clamp(m.zoom + d, 1, #ZOOMS)
    else
      local spp = ZOOMS[m.zoom]
      MT.seek(util.clamp(m.status.pos + d * 4 * spp, 0, m.status.max))
    end
  end
  refresh_status()
  _menu.redraw()
end

--- drawing

local function draw_header(s)
  -- selected track box
  screen.level(15)
  screen.rect(0.5, 2.5, 9, 9)
  screen.stroke()
  screen.move(5, 9)
  screen.text_center(m.track)

  local state
  if s.recording then state = "REC"
  elseif s.playing then state = "PLAY"
  elseif s.armed > 0 then state = "ARM " .. s.armed
  else state = "STOP" end
  screen.level(s.recording and 15 or 6)
  screen.move(14, 9)
  screen.text(state)

  local msg
  if s.mem_full then msg = "MEM FULL"
  elseif m.msg and now() < m.msg_until then msg = m.msg
  elseif s.busy > 0 then msg = "BUSY" end
  if msg then
    screen.level(4)
    screen.move(78, 9)
    screen.text_right(msg)
  end

  screen.level(15)
  screen.move(127, 9)
  screen.text_right(format_time(s.pos))
end

local function draw_ruler(s, spp)
  -- pick a tick spacing of at least 8 pixels
  local tick = TICKS[#TICKS]
  for _, t in ipairs(TICKS) do
    if t / spp >= 8 then tick = t; break end
  end
  local t0 = s.pos - PLAYHEAD_X * spp
  local first = math.ceil(math.max(0, t0) / tick)
  local last = math.floor(math.min(s.max, s.pos + (128 - PLAYHEAD_X) * spp) / tick)
  for i = first, last do
    local x = math.floor((i * tick - t0) / spp) + 0.5
    local major = i % 4 == 0
    screen.level(major and 4 or 2)
    screen.move(x, 15)
    screen.line(x, major and 19 or 17)
    screen.stroke()
  end
end

local function draw_tracks(s, spp)
  -- first column at or after tape start
  local x0 = math.max(0, math.ceil(PLAYHEAD_X - s.pos / spp))
  local n = 128 - x0
  local start = s.pos + (x0 - PLAYHEAD_X) * spp
  for t = 1, MT.TRACKS do
    local y = TRACK_Y[t]
    local selected = t == m.track
    local muted = params:get("mtape_mute_" .. t) == 1

    -- tape baseline
    screen.level(1)
    screen.move(x0, y + 0.5)
    screen.line(128, y + 0.5)
    screen.stroke()

    if n > 0 then
      local peaks = MT.overview(t, start, spp, n)
      local lvl = muted and 2 or (selected and 15 or 6)
      screen.level(lvl)
      for i = 1, n do
        local p = peaks[i]
        if p > 0 then
          local h = math.max(1, math.floor(math.sqrt(p) * 4 + 0.5))
          local x = x0 + i - 0.5
          screen.move(x, y + 1 - h)
          screen.line(x, y + h)
          screen.stroke()
        end
      end
    end

    -- track marker: armed tracks blink
    if s.armed == t then
      if math.floor(now() * 4) % 2 == 0 or s.recording then
        screen.level(15)
        screen.rect(0, y - 1, 2, 3)
        screen.fill()
      end
    elseif selected then
      screen.level(10)
      screen.rect(0, y, 2, 1)
      screen.fill()
    end
  end
end

m.redraw = function()
  if not m.status then refresh_status() end
  local s = m.status
  local spp = ZOOMS[m.zoom]
  screen.clear()
  screen.aa(0)
  screen.line_width(1)
  screen.font_face(1)
  screen.font_size(8)
  draw_header(s)
  draw_ruler(s, spp)
  draw_tracks(s, spp)
  -- playhead
  screen.level(s.recording and 15 or 8)
  screen.move(PLAYHEAD_X + 0.5, 13)
  screen.line(PLAYHEAD_X + 0.5, 64)
  screen.stroke()
  _menu.draw_panel()
  screen.update()
end

m.init = function()
  refresh_status()
  _menu.timer.time = 1 / FPS
  _menu.timer.count = -1
  _menu.timer.event = function()
    if m.modal then return end
    refresh_status()
    _menu.redraw()
  end
  _menu.timer:start()
end

m.deinit = function()
  _menu.timer:stop()
end

return m
