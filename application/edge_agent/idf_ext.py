import importlib.util
import os
import sys


def _load_module(name, path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def action_extensions(base_actions, project_dir):
    bmgr_dir = os.path.join(project_dir, "components", "espressif__esp_board_manager")
    bmgr_idf_ext = os.path.join(bmgr_dir, "idf_ext.py")
    gen_script = os.path.join(bmgr_dir, "gen_bmgr_config_codes.py")
    gen_dir = os.path.dirname(gen_script)
    if gen_dir not in sys.path:
        sys.path.insert(0, gen_dir)
    bmgr_ext = _load_module("esp_bmgr_idf_ext", bmgr_idf_ext)
    return bmgr_ext.action_extensions(base_actions, project_dir)
