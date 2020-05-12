cmake_minimum_required (VERSION 2.8.12)
project (obs-shaderfilter)

set(obs-shaderfilter_SOURCES
	obs-shaderfilter.c
	)

add_library(obs-shaderfilter MODULE
	${obs-shaderfilter_SOURCES})
target_link_libraries(obs-shaderfilter
	libobs)

install_obs_plugin_with_data(obs-shaderfilter data)
