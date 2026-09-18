local M = {}
export('walk_speed', 3.0)
function M.update(self, dt)
  local fwd = 0
  if Input.key_down(Key.W) then fwd = fwd + 1 end
  if Input.key_down(Key.S) then fwd = fwd - 1 end
  local dx = fwd * self.walk_speed * dt
  if dx ~= 0 then
    local x, y, z = self:position()
    z = z - dx
    self:set_position(x, y, z)
  end
end
return M
