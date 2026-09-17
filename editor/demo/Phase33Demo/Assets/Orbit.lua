local M = {}
export('n', 0)
function M.update(self, dt)
  self.n = self.n + 1
end
return M
