"""CPU regression: time spheres follow the game's camera translation convention."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import evaluate_zombie_routes as routes


def function(source, name):
    match = re.search(r"^(?:static )?(?:inline )?[\w* ]+ " + name + r"\([^;]*?\)[^\n{]*\s*\{", source, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


gte = (ROOT / "src/game/GteMatrix.cpp").read_text()
random = (ROOT / "src/game/mods/ZombieRandom.cpp").read_text()
source = r"""
#include <cassert>
#include <cmath>
#include <cstdio>
struct MATRIX { short m[3][3]; short pad; int t[3]; };
struct VECTOR { int x,y,z,pad; };
struct TimePoint { float x,y,z; };
static MATRIX g_RoomCameraData;
"""
for name in ("GteFixedMul12", "ApplyMatrixLV", "MatrixToCamera"):
    source += function(gte, name)
source += function(random, "time_view_point")
source += r"""
int main() {
    // Camera cuts with different horizontal directions, pitches and heights.
    int cameras[][8] = {
        {10000,-4000,5000,12000,0,15000,0,0},
        {22000,-6000,14000,12000,0,15000,0,0},
        {12000,-8000,24000,12000,-1000,15000,0,0},
        {3000,-3000,17000,12000,-2000,15000,0,0},
        {12000,-1000,3000,12000,-1000,15000,0,0}
    };
    for (auto& camera : cameras) {
        MatrixToCamera((MATRIX*)camera);
        TimePoint target={(float)camera[3],(float)camera[4],(float)camera[5]};
        TimePoint centre=time_view_point(target);
        // The look-at point stays at screen centre after every camera cut.
        assert(centre.z>512);
        assert(std::fabs(centre.x*256/centre.z)<0.2f);
        assert(std::fabs(centre.y*256/centre.z)<0.2f);
        for(int y : {0,-1000,-3600}) {
            for(int x : {10000,12000,14000}) {
                VECTOR world={x,y,15000,0}, rotated={};
                ApplyMatrixLV(&g_RoomCameraData,&world,&rotated);
                TimePoint actual=time_view_point({(float)x,(float)y,15000});
                // Compare sphere vertices to the translation used for models,
                // allowing only the original fixed-point truncation difference.
                assert(std::fabs(actual.x-(rotated.x+g_RoomCameraData.t[0]))<3.1f);
                assert(std::fabs(actual.y-(rotated.y+g_RoomCameraData.t[1]))<3.1f);
                assert(std::fabs(actual.z-(rotated.z+g_RoomCameraData.t[2]))<3.1f);
            }
        }
    }
    puts("Time sphere projection OK: five camera cuts, tilted views and elevated floors match game transforms.");
}
"""
source = "#include <initializer_list>\n" + source
with tempfile.TemporaryDirectory(prefix="re1-time-projection-") as temp:
    with patch.object(routes, "adapter_source", return_value=source):
        executable = routes.build_adapter(Path(temp))
    subprocess.run([str(executable)], check=True)
