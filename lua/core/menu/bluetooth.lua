local listselect = require 'listselect'

local m = {
  pos = 0,
  list = {"scan","connect","discon","del"},
}

m.len = #m.list

m.connect = function(x)
  if x ~= "cancel" then
    bluetooth.connect(x, function() _menu.redraw() end)
  end
  _menu.redraw()
end

m.disconnect = function(x)
  if x ~= "cancel" then
    bluetooth.disconnect(x, function() _menu.redraw() end)
  end
  _menu.redraw()
end

m.del = function(x)
  if x ~= "cancel" then
    bluetooth.forget(x, function() _menu.redraw() end)
  end
  _menu.redraw()
end

m.key = function(n,z)
  if n==2 and z==1 then
    _menu.set_page("SYSTEM")
  elseif n==3 and z==1 then
    if m.pos == 0 then
      bluetooth.scan(function() _menu.redraw() end)
      _menu.redraw()
    elseif m.pos == 1 then
      listselect.enter(bluetooth.device_names(), m.connect)
    elseif m.pos == 2 then
      listselect.enter(bluetooth.connected, m.disconnect)
    elseif m.pos == 3 then
      listselect.enter(bluetooth.device_names(), m.del)
    end
  end
end

m.enc = function(n,delta)
  if n==2 then
    m.pos = util.clamp(m.pos + delta, 0, m.len - 1)
    _menu.redraw()
  end
end

m.redraw = function()
  screen.clear()
  screen.level(4)

  screen.move(0,10)
  screen.text("STATUS: " .. bluetooth.status)
  screen.move(0,20)
  screen.text("FOUND: " .. #bluetooth.devices)
  screen.move(0,30)
  local midi = "MIDI: " .. table.concat(bluetooth.connected, ", ")
  screen.text(util.trim_string_to_width(midi, 128))

  local xp = {0,26,68,104}
  for i=1,m.len do
    screen.move(xp[i],60)
    local line = m.list[i]
    if(i==m.pos+1) then
      screen.level(15)
    else
      screen.level(4)
    end
    screen.text(string.upper(line))
  end

  screen.update()
end

m.init = function()
  bluetooth.on()
  bluetooth.update(function() _menu.redraw() end)
  _menu.timer.time = 3
  _menu.timer.count = -1
  _menu.timer.event = function()
    bluetooth.update(function() _menu.redraw() end)
  end
  _menu.timer:start()
end

m.deinit = function()
  _menu.timer:stop()
end

return m
