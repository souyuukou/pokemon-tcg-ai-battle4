import json
from pathlib import Path
p = Path(r"C:\Users\SOU YAMAGUCHI\Downloads\87096749.json")
d = json.loads(p.read_text(encoding="utf-8"))
steps = d["steps"]
specs = [(0, [11,34,50,63,103,132,150,166]), (1, [5,23,47,60,73,123,135,153])]
out_dir = Path(r"D:\pokemon-tcg-ai-battle4\sample_submission\tests\fixtures\replay_87096749")
out_dir.mkdir(parents=True, exist_ok=True)
manifest = []
for agent, steps_list in specs:
    for i in steps_list:
        obs = steps[i][agent]["observation"]
        sel = obs.get("select") or {}
        cur = obs.get("current") or {}
        name = "agent%d_step%03d.json" % (agent, i)
        (out_dir / name).write_text(json.dumps(obs, ensure_ascii=False), encoding="utf-8")
        item = {"file": name, "agent": agent, "step": i, "turn": cur.get("turn"), "context": sel.get("context"), "selectType": sel.get("type"), "options": len(sel.get("option") or [])}
        manifest.append(item)
        print(item)
(out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
print("wrote", len(manifest), "fixtures to", out_dir)
