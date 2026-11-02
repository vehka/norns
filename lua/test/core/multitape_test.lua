-- lua/test/core/multitape_test.lua
-- unit tests for audio.multitape (4-track tape controls)

local luaunit = require('lib/test/luaunit')
local mock = require('lib/test/mock')

local function setup_env(opts)
  opts = opts or {}
  norns = {
    disk = 1000,
    state = {
      mtape = {
        level = -6,
        rev = -math.huge,
        source = 3,
        track_level = {0, -6, 0, 0},
        track_pan = {0, 0.5, 0, -1},
        track_mute = {0, 0, 1, 0},
        loop = 1,
        loop_start = 1.5,
        loop_end = 3,
        grid = opts.grid or 1,
        bar = 4,
        jog_nudge = 2,
        jog_accel = 3,
        jog_stop = 0.5,
      },
    },
  }
  _path = { audio = '/dust/audio/' }

  local made = {}
  util = {
    time = function() return opts.time or 100 end,
    clamp = function(x, lo, hi) return math.min(hi, math.max(lo, x)) end,
    dbamp = function(db) return 10 ^ (db / 20) end,
    make_dir = function(p) made[#made + 1] = p end,
    file_exists = function(p) return opts.existing and opts.existing[p] or false end,
  }

  _norns = {}
  for _, name in ipairs({
    'mtape_play', 'mtape_seek', 'mtape_arm', 'mtape_rec', 'mtape_track_level',
    'mtape_track_pan', 'mtape_track_mute', 'mtape_source', 'mtape_level',
    'mtape_level_rev', 'mtape_load', 'mtape_save', 'mtape_clear', 'mtape_status',
    'mtape_overview', 'mtape_rate', 'mtape_rate_slew', 'mtape_loop',
    'mtape_loop_points', 'mtape_mixdown', 'mtape_jog', 'mtape_nudge', 'mtape_jog_stop',
  }) do
    _norns[name] = mock.spy()
  end
  _norns.mtape_status = mock.spy(function() return { playing = opts.playing or false } end)

  clock = { get_beat_sec = function() return 0.5 end }
  package.loaded['controlspec'] = { new = function() return {} end }
  package.loaded['core/audio'] = nil
  local audio = require('core/audio')
  return { audio = audio, made = made }
end

TestMultitape = {}

function TestMultitape.test_transport_and_arm()
  local env = setup_env()
  local MT = env.audio.multitape

  MT.play()
  luaunit.assertEquals(_norns.mtape_play.args(1)[1], true)
  MT.stop()
  luaunit.assertEquals(_norns.mtape_play.args(2)[1], false)

  MT.seek(-3)
  luaunit.assertEquals(_norns.mtape_seek.args(1)[1], 0)

  MT.arm()
  luaunit.assertEquals(_norns.mtape_arm.args(1)[1], 0)
  MT.arm(2)
  luaunit.assertEquals(_norns.mtape_arm.args(2)[1], 2)

  MT.rec(nil)
  luaunit.assertEquals(_norns.mtape_rec.args(1)[1], false)
end

function TestMultitape.test_restore_pushes_saved_state()
  local env = setup_env()
  local MT = env.audio.multitape

  MT.restore()
  luaunit.assertAlmostEquals(_norns.mtape_level.args(1)[1], 10 ^ (-6 / 20), 1e-9)
  luaunit.assertEquals(_norns.mtape_level_rev.args(1)[1], 0)
  luaunit.assertEquals(_norns.mtape_source.args(1)[1], 3)
  luaunit.assertEquals(_norns.mtape_track_level.call_count(), 4)
  luaunit.assertEquals(_norns.mtape_track_pan.args(2)[2], 0.5)
  luaunit.assertEquals(_norns.mtape_track_mute.args(3)[2], true)
  luaunit.assertEquals(_norns.mtape_track_mute.args(1)[2], false)
  luaunit.assertEquals(_norns.mtape_loop_points.args(1)[1], 1.5)
  luaunit.assertEquals(_norns.mtape_loop_points.args(1)[2], 3)
  luaunit.assertEquals(_norns.mtape_loop.args(1)[1], true)
end

function TestMultitape.test_mixdown_path()
  setup_env()
  local MT = require('core/audio').multitape
  luaunit.assertEquals(MT.mixdown('take'), '/dust/audio/multitape/take.wav')
  luaunit.assertEquals(_norns.mtape_mixdown.args(1)[1], '/dust/audio/multitape/take.wav')
end

function TestMultitape.test_snap_follows_grid_param()
  local env = setup_env({ grid = 1 })
  luaunit.assertEquals(env.audio.multitape.snap(1.3), 1.3)
  env = setup_env({ grid = 2 })
  -- 0.5 s beats
  luaunit.assertEquals(env.audio.multitape.snap(1.3), 1.5)
  luaunit.assertEquals(env.audio.multitape.snap(1.2), 1.0)
end

function TestMultitape.test_save_uses_audio_folder()
  local env = setup_env()
  local MT = env.audio.multitape

  local dir = MT.save('song')
  luaunit.assertEquals(dir, '/dust/audio/multitape/song')
  luaunit.assertEquals(env.made[1], '/dust/audio/multitape/')
  luaunit.assertEquals(_norns.mtape_save.args(1)[1], '/dust/audio/multitape/song')
end

function TestMultitape.test_open_loads_existing_tracks_and_clears_all()
  local env = setup_env({ existing = { ['/dust/audio/multitape/song/track2.wav'] = true } })
  local MT = env.audio.multitape

  MT.open('song')
  luaunit.assertEquals(_norns.mtape_clear.call_count(), 4)
  luaunit.assertEquals(_norns.mtape_load.call_count(), 1)
  local args = _norns.mtape_load.args(1)
  luaunit.assertEquals(args[1], 2)
  luaunit.assertEquals(args[2], '/dust/audio/multitape/song/track2.wav')
  luaunit.assertEquals(args[3], 0)
end

function TestMultitape.test_overview_clamps_start()
  setup_env()
  local MT = require('core/audio').multitape
  MT.overview(1, -2, 0.1, 64)
  local args = _norns.mtape_overview.args(1)
  luaunit.assertEquals(args[2], 0)
  luaunit.assertEquals(args[4], 64)
end

function TestMultitape.test_jog_turn_enters_jog_and_pushes()
  local env = setup_env({ playing = true })
  local MT = env.audio.multitape
  MT.jog_turn(1)
  luaunit.assertTrue(MT.jogging())
  luaunit.assertEquals(_norns.mtape_rate.args(1)[1], 0)
  luaunit.assertEquals(_norns.mtape_jog.args(1)[1], true)
  luaunit.assertEquals(_norns.mtape_play.args(1)[1], true)
  luaunit.assertEquals(_norns.mtape_jog_stop.args(1)[1], 0.5)
  -- a lone tick gets no speed boost: push = 2 ms / 0.5 s
  local push = (2 / 1000) / 0.5
  luaunit.assertAlmostEquals(_norns.mtape_nudge.args(1)[1], push, 1e-9)
end

function TestMultitape.test_jog_close_ticks_dont_spike()
  local env = setup_env({ playing = true })
  local MT = env.audio.multitape
  -- two ticks at the same instant (util.time is constant here)
  MT.jog_turn(1)
  MT.jog_turn(1)
  local second = _norns.mtape_nudge.args(2)[1]
  local lone = (2 / 1000) / 0.5
  luaunit.assertTrue(second > lone)
  luaunit.assertTrue(second < lone * 5)
end

function TestMultitape.test_jog_end_restores_playback()
  local env = setup_env({ playing = true })
  local MT = env.audio.multitape
  MT.jog_turn(-2)
  MT.jog_end()
  luaunit.assertFalse(MT.jogging())
  luaunit.assertEquals(_norns.mtape_jog.args(2)[1], false)
  luaunit.assertEquals(_norns.mtape_rate.args(2)[1], 1)
  -- was playing before the jog: keeps playing
  luaunit.assertEquals(_norns.mtape_play.args(2)[1], true)
end

function TestMultitape.test_stop_leaves_jog()
  local env = setup_env({ playing = false })
  local MT = env.audio.multitape
  MT.jog_turn(1)
  MT.stop()
  luaunit.assertFalse(MT.jogging())
  luaunit.assertEquals(_norns.mtape_play.args(2)[1], false)
end
