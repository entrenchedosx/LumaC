-- Conductor: alternates the arm between wave and nod with a
-- crossfade, and reports playback time. Clip IDs are baked in by
-- main.c (WAVE_HEX / NOD_HEX placeholders) — see the fallback in
-- main.c when running without --scripts.
local wave = nil
local nod = nil
local timer = 0.0
local showing_wave = true

function start(self)
  wave = Assets.find_by_id("WAVE_HEX")
  nod = Assets.find_by_id("NOD_HEX")
  if wave == nil or nod == nil then
    print("[conductor] missing clips")
    return
  end
  self:animation_play(wave)
  print("[conductor] playing wave")
end

function update(self, dt)
  if wave == nil or nod == nil then return end
  timer = timer + dt
  if timer >= 4.0 then
    timer = 0.0
    showing_wave = not showing_wave
    if showing_wave then
      self:animation_crossfade(wave, 0.5)
      print("[conductor] crossfade -> wave")
    else
      self:animation_crossfade(nod, 0.5)
      print("[conductor] crossfade -> nod")
    end
  end
  if self:animation_is_playing() then
    local t = self:animation_time()
    local d = self:animation_duration()
    if t >= 0 and d > 0 and (timer - dt) < 1.0 and timer >= 1.0 then
      print(string.format("[conductor] t=%.2f/%.2f", t, d))
    end
  end
end
