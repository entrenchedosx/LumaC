local M = {}
export('t', 0)
export('radius', 1.5)
export('speed', 1.0)
function M.start(self)
  self.t = 0
end
function M.update(self, dt)
  self.t = self.t + dt * self.speed
  local x, y, z = self:position()
  x = self.radius * math.cos(self.t)
  z = self.radius * math.sin(self.t)
  self:set_position(x, y, z)
end
return M
