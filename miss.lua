request = function()
   local path = "/index.html?nocache=" .. math.random(1, 100000000)
   return wrk.format("GET", path)
end
