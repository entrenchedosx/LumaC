local M = {}
export('hits', 0)
function M.on_trigger_enter(self, other)
  self.hits = self.hits + 1
end
return M
