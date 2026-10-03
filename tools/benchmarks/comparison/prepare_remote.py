"""Extract upstream RemoteImGui's serializer without its obsolete UI/network API."""
from pathlib import Path
import sys
import re
source = Path(sys.argv[1]).read_text()
# Keep RemoteImGui's own LZ4 implementation. Prefix identifiers only, so it
# can coexist with the production encoder's newer LZ4 in the same executable.
lz4_out = Path(sys.argv[2]).parent/'remote_lz4'
lz4_out.mkdir(exist_ok=True)
for name in ['lz4.c','lz4.h']:
    upstream = (Path(sys.argv[1]).parent/'lz4'/name).read_text()
    (lz4_out/name).write_text(re.sub(r'\bLZ4_', 'RemoteLZ4_', upstream))
parts = source[source.index('#pragma pack(1)'):source.index('\n\t};', source.index('void PreparePacketFrame'))]
a = source.index('\t\t\tunsigned int totalSize', source.index('void RemoteDraw'))
b = source.index('\n\t\t\tGServer.SendPacket();', a) + len('\n\t\t\tGServer.SendPacket();')
body = source[a:b]
Path(sys.argv[2]).write_text('''// Generated from pinned RemoteImGui; see upstream MIT license.
#include "remote_lz4/lz4.h"
#include <cassert>
namespace remote_codec {
constexpr int IMGUI_REMOTE_KEY_FRAME=60;
struct WebSocketServer {
    std::vector<unsigned char> Packet, PrevPacket;
    int Frame=0, PrevPacketSize=0;
    bool IsKeyFrame=true, ForceKeyFrame=true, Supports32BitIndexBuffers=false;
    size_t sent=0;
    void SendBinary(const void*, int size) { sent+=size; }
''' + parts.replace('LZ4_compress_limitedOutput', 'RemoteLZ4_compress_limitedOutput') + '''
};
inline size_t encode(WebSocketServer& GServer, ImDrawData* data) {
    auto* cmd_lists=data->CmdLists.Data;
    int cmd_lists_count=data->CmdListsCount;
    GServer.sent=0;
''' + body + '''
    ++GServer.Frame;
    return GServer.sent;
}
}
''')
