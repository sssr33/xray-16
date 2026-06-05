function normal(shader, t_base, t_second, t_detail)
    shader:begin("combine_1", "clouds_volumetric_test")
        : fog(false)
        : zb(false, false)
        : blend(true, blend.srcalpha, blend.invsrcalpha)
        : sorting(3, true)
    shader:sampler("s_tonemap") :texture("$user$tonemap")
end
