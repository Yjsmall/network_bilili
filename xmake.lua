set_project("HTTP Server")
set_languages("c99", "cxx17")
add_requires("fmt")

add_rules("mode.debug", "mode.release")
target("server")
  set_kind("binary")
  add_files("src/server.cc")
  add_packages("fmt")
