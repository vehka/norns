-- menu/multitape.lua
-- 4TRK: a 4-track tape view in the spirit of the OP-1 tape.
-- the playhead is fixed in the centre and the tape scrolls past it.
--
-- E2 select track          K2 play / stop
-- E3 jump (beats w/ grid)  K3 record-arm selected track (punch in/out while playing)
-- K1+E1 scrub with inertia (PARAMS > MULTITAPE > scrub nudge / acceleration / stop time)
-- K1+E2 zoom               K1+K2 back to loop start (or tape start)
-- K1+E3 cue: hold K1 and turn for ff / rewind speed; release K1 to go back
-- K1+K3 track / loop / project actions

local fileselect = require 'fileselect'
local listselect = require 'listselect'
local textentry = require 'textentry'

local MT = audio.multitape

local PLAYHEAD_X = 64
local LOOP_Y = 21
local TRACK_Y = {28, 38, 48, 58}
local ZOOMS = {1/128, 1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1, 2, 3} -- seconds per pixel
local TICKS = {0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60}             -- seconds
local BEAT_TICKS = {1/4, 1/2, 1, 2, 4, 8, 16, 32, 64}            -- beats
local SHUTTLE = {-16, -8, -4, -2, -1, -0.5, 0.5, 1, 2, 4, 8, 16} -- rates
local SHUTTLE_NORMAL = 8
local MENU_E1_SENS = 8   -- menu.lua's E1 sensitivity, restored after scrubbing
local FPS = 20

local m = {
  track = 1,
  zoom = 4,
  status = nil,
  modal = false,
  msg = nil,
  msg_until = 0,
  shuttle = nil, -- {idx, was_playing} while K1+E3 cues
  scrub = false, -- true while K1+E1 scrubs
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

local function grid_on() return params:get("mtape_grid") == 2 end

local function refresh_status()
  m.status = MT.status()
end

-- record-arm is also the record switch: armed + playing = recording
local function set_armed(track)
  MT.arm(track)
  MT.rec(track > 0)
  params:set("mtape_arm", track + 1, true) -- keep the param in step, silently
end

local function end_shuttle()
  if not m.shuttle then return end
  MT.rate(1)
  if not m.shuttle.was_playing then MT.stop() end
  m.shuttle = nil
end

local function scrub(d)
  if not m.scrub then
    m.scrub = true
    norns.encoders.set_sens(1, 1)
  end
  MT.jog_turn(d)
end

local function end_scrub()
  if not m.scrub then return end
  m.scrub = false
  MT.jog_end()
  norns.encoders.set_sens(1, MENU_E1_SENS)
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

local function name_prompt(heading, default, fn)
  modal(function()
    textentry.enter(function(name)
      if name and name ~= "" then fn(name) end
      end_modal()
    end, default, heading)
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

local function set_loop_point(which)
  local t = MT.snap(m.status.pos)
  local s, e = params:get("mtape_loop_start"), params:get("mtape_loop_end")
  if which == "in" then s = t else e = t end
  if e < s then s, e = e, s end
  params:set("mtape_loop_start", s)
  params:set("mtape_loop_end", e)
  flash(string.format("LOOP %s-%s", format_time(s), format_time(e)))
end

local function actions()
  local t = m.track
  local muted = params:get("mtape_mute_" .. t) == 1
  local looping = params:get("mtape_loop") == 1
  local items = {
    {"load file at start", function() load_file(false); return true end},
    {"load file at playhead", function() load_file(true); return true end},
    {(muted and "unmute" or "mute") .. " track " .. t, function()
      params:set("mtape_mute_" .. t, muted and 0 or 1)
    end},
    {"clear track " .. t, function()
      if m.status.armed == t then set_armed(0) end
      MT.clear(t)
      flash("CLEARED " .. t)
    end},
    {"loop in = playhead", function() set_loop_point("in") end},
    {"loop out = playhead", function() set_loop_point("out") end},
    {looping and "loop off" or "loop on", function()
      params:set("mtape_loop", looping and 0 or 1)
    end},
    {"grid " .. (grid_on() and "off" or "on"), function()
      params:set("mtape_grid", grid_on() and 1 or 2)
    end},
    {"mix down", function()
      name_prompt("mix down as:", "mix-" .. os.date("%y%m%d-%H%M"), function(name)
        MT.mixdown(name)
        flash("MIXING " .. name)
      end)
      return true
    end},
    {"save 4trk", function()
      name_prompt("save 4trk as:", os.date("%y%m%d-%H%M"), function(name)
        MT.save(name)
        flash("SAVED " .. name)
      end)
      return true
    end},
    {"open 4trk", function() open_project(); return true end},
  }
  local labels = {}
  for i, item in ipairs(items) do labels[i] = item[1] end
  modal(function()
    listselect.enter(labels, function(choice)
      local deferred = false
      for _, item in ipairs(items) do
        if item[1] == choice then deferred = item[2]() == true end
      end
      if not deferred then end_modal() end
    end)
  end)
end

--- input

m.key = function(n, z)
  if not m.status then return end
  -- releasing K1 ends a cue or scrub
  if n == 1 and z == 0 then
    end_shuttle()
    end_scrub()
    return
  end
  if z ~= 1 then return end
  local s = m.status
  if n == 2 then
    if _menu.alt then
      MT.seek(s.loop and s.loop_start or 0)
    elseif s.playing then
      MT.stop()
    else
      MT.play()
    end
  elseif n == 3 then
    if _menu.alt then
      actions()
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
  if n == 1 then
    -- E1 alone changes menu screens; it only reaches us with K1 held
    scrub(d)
  elseif n == 2 then
    if _menu.alt then
      m.zoom = util.clamp(m.zoom + d, 1, #ZOOMS)
    else
      m.track = util.clamp(m.track + d, 1, MT.TRACKS)
    end
  elseif n == 3 then
    if _menu.alt then
      if not m.shuttle then
        m.shuttle = {idx = SHUTTLE_NORMAL, was_playing = m.status.playing}
      end
      m.shuttle.idx = util.clamp(m.shuttle.idx + d, 1, #SHUTTLE)
      MT.rate(SHUTTLE[m.shuttle.idx])
      if not m.status.playing then MT.play() end
    elseif grid_on() then
      local beat = MT.beat_sec()
      MT.seek(util.clamp(MT.snap(m.status.pos) + d * beat, 0, m.status.max))
    else
      local spp = ZOOMS[m.zoom]
      MT.seek(util.clamp(m.status.pos + d * 4 * spp, 0, m.status.max))
    end
  end
  refresh_status()
  _menu.redraw()
end

--- drawing

-- x position of a tape time
local function tx(s, t, spp) return PLAYHEAD_X + (t - s.pos) / spp end

local function draw_header(s)
  -- selected track box
  screen.level(15)
  screen.rect(0.5, 2.5, 9, 9)
  screen.stroke()
  screen.move(5, 9)
  screen.text_center(m.track)

  local state
  if MT.jogging() then state = "SCRUB"
  elseif s.recording and s.rate == 1 then state = "REC"
  elseif s.playing and math.abs(s.rate - 1) > 0.01 then
    local r = math.abs(s.rate)
    local fmt = r < 1 and "%s%.1fx" or "%s%dx"
    state = string.format(fmt, s.rate < 0 and "<<" or ">>", r < 1 and r or math.floor(r + 0.5))
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
    screen.move(84, 9)
    screen.text_right(msg)
  end

  screen.level(15)
  screen.move(127, 9)
  screen.text_right(format_time(s.pos))
end

local function draw_ruler(s, spp)
  local t0 = s.pos - PLAYHEAD_X * spp
  local t1 = s.pos + (128 - PLAYHEAD_X) * spp
  local tick, major_every
  if grid_on() then
    -- beat grid from the clock tempo; bars are major ticks
    local beat = MT.beat_sec()
    local bar = params:get("mtape_bar")
    local beats = BEAT_TICKS[#BEAT_TICKS]
    for _, b in ipairs(BEAT_TICKS) do
      if b * beat / spp >= 6 then beats = b; break end
    end
    tick = beats * beat
    major_every = math.max(1, math.floor(bar / beats + 0.5))
  else
    tick = TICKS[#TICKS]
    for _, t in ipairs(TICKS) do
      if t / spp >= 8 then tick = t; break end
    end
    major_every = 4
  end
  local first = math.ceil(math.max(0, t0) / tick)
  local last = math.floor(math.min(s.max, t1) / tick)
  for i = first, last do
    local x = math.floor(tx(s, i * tick, spp)) + 0.5
    local major = i % major_every == 0
    screen.level(major and 4 or 2)
    screen.move(x, 13)
    screen.line(x, major and 17 or 15)
    screen.stroke()
  end
end

local function draw_loop(s, spp)
  if s.loop_end <= s.loop_start then return end
  local x0 = math.max(0, math.floor(tx(s, s.loop_start, spp)))
  local x1 = math.min(128, math.floor(tx(s, s.loop_end, spp)))
  if x1 <= 0 or x0 >= 128 then return end
  screen.level(s.loop and 10 or 2)
  screen.rect(x0, LOOP_Y - 1, math.max(1, x1 - x0), 2)
  screen.fill()
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
      screen.level(muted and 2 or (selected and 15 or 6))
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
  draw_loop(s, spp)
  draw_tracks(s, spp)
  -- playhead
  screen.level(s.recording and 15 or 8)
  screen.move(PLAYHEAD_X + 0.5, 12)
  screen.line(PLAYHEAD_X + 0.5, 64)
  screen.stroke()
  _menu.draw_panel()
  screen.update()
end

m.init = function()
  -- a dialog left open by switching away from the menu never calls back
  m.modal = false
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
  m.modal = false
  end_shuttle()
  end_scrub()
  _menu.timer:stop()
end

return m
