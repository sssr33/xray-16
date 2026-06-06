function normal(shader, t_base, t_second, t_detail)
    shader:begin("sky2", "clouds_volumetric_test")
        : fog(false)
        : zb(false, false)
        : blend(false, blend.one, blend.zero)
        : sorting(3, true)
end
