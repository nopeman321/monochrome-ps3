import json
import os
import re

LOG_FILE = r"C:\Users\Juanjo\.gemini\antigravity\brain\be8deae7-c81d-4c99-8ae1-7a89d3dcec2f\.system_generated\logs\transcript_full.jsonl"
OUT_PROJECT = r"C:\Users\Juanjo\.gemini\antigravity\scratch\ps3-monochrome\CONVERSACION_COMPLETA.txt"
OUT_DESKTOP = r"C:\Users\Juanjo\Desktop\CONVERSACION_MONOCHROME_PS3.txt"

def clean_system_tags(text):
    if not text:
        return ""
    # Remove XML-style metadata wrappers if any
    cleaned = re.sub(r'<USER_REQUEST>\s*', '', text)
    cleaned = re.sub(r'\s*</USER_REQUEST>', '', cleaned)
    cleaned = re.sub(r'<ADDITIONAL_METADATA>.*?</ADDITIONAL_METADATA>', '', cleaned, flags=re.DOTALL)
    cleaned = re.sub(r'<CONTEXT_SUMMARY>.*?</CONTEXT_SUMMARY>', '[Context Summary Resumed]\n', cleaned, flags=re.DOTALL)
    # Redact any GitHub personal/OAuth tokens or keys
    cleaned = re.sub(r'gh[pousr]_[A-Za-z0-9_]{20,}', '[REDACTED_GITHUB_TOKEN]', cleaned)
    cleaned = re.sub(r'password=gh[pousr]_[^\s]+', 'password=[REDACTED]', cleaned)
    return cleaned.strip()

def export_transcript():
    if not os.path.exists(LOG_FILE):
        print(f"Error: {LOG_FILE} not found!")
        return

    print(f"[+] Reading transcript from: {LOG_FILE}")
    
    entries = []
    with open(LOG_FILE, 'r', encoding='utf-8', errors='replace') as f:
        for line_num, line in enumerate(f):
            line = line.strip()
            if not line:
                continue
            try:
                data = json.loads(line)
            except Exception as e:
                continue
            
            step_idx = data.get("step_index", line_num)
            source = data.get("source", "")
            step_type = data.get("type", "")
            created_at = data.get("created_at", "")
            content = data.get("content", "")
            tool_calls = data.get("tool_calls", [])

            # We care about:
            # 1. USER_INPUT / USER_EXPLICIT
            # 2. PLANNER_RESPONSE (Assistant text content)
            # 3. Tool calls summary (actions taken)

            if source == "USER_EXPLICIT" or step_type == "USER_INPUT":
                user_msg = clean_system_tags(content)
                if user_msg:
                    entries.append({
                        "role": "USUARIO",
                        "time": created_at,
                        "text": user_msg,
                        "tools": []
                    })
            elif source == "MODEL" or step_type == "PLANNER_RESPONSE":
                cleaned_content = clean_system_tags(content) if content else ""
                tool_summaries = []
                for tc in tool_calls:
                    fn_name = tc.get("function", {}).get("name", "tool")
                    args = tc.get("function", {}).get("arguments", {})
                    summary = args.get("toolSummary") or args.get("toolAction") or fn_name
                    target = args.get("TargetFile") or args.get("CommandLine") or args.get("query") or args.get("Url") or ""
                    target = clean_system_tags(str(target))
                    if target and len(target) > 80:
                        target = target[:77] + "..."
                    tool_summaries.append(f"{summary} ({target})" if target else summary)

                if cleaned_content or tool_summaries:
                    entries.append({
                        "role": "ASISTENTE (ANTIGRAVITY)",
                        "time": created_at,
                        "text": cleaned_content,
                        "tools": tool_summaries
                    })

    print(f"[+] Total formatted interaction turns: {len(entries)}")

    lines = []
    lines.append("=" * 80)
    lines.append("TRANSCRIPCIÓN COMPLETA DE LA CONVERSACIÓN — MONOCHROME PS3 PORT")
    lines.append("Proyecto: Port de Monochrome Music a PlayStation 3 (Cell B.E. / GameOS / PS3HEN)")
    lines.append("Repositorio: https://github.com/nopeman321/monochrome-ps3")
    lines.append("=" * 80)
    lines.append("\n")

    for idx, e in enumerate(entries, 1):
        timestamp = e["time"][:19].replace("T", " ") if e["time"] else ""
        lines.append(f"[{idx}] ──────────────────────────────────────────────────────────")
        lines.append(f"REMITENTE: {e['role']}  |  FECHA/HORA: {timestamp}")
        lines.append("───────────────────────────────────────────────────────────────")
        
        if e["tools"]:
            lines.append("ACCIONES REALIZADAS:")
            for t in e["tools"]:
                lines.append(f"  • {t}")
            lines.append("")

        if e["text"]:
            lines.append("MENSAJE:")
            lines.append(e["text"])
            lines.append("")
        lines.append("\n")

    output_text = "\n".join(lines)

    for out_path in [OUT_PROJECT, OUT_DESKTOP]:
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        with open(out_path, 'w', encoding='utf-8') as f:
            f.write(output_text)
        print(f"[OK] Saved text transcript ({len(output_text)} chars) -> {out_path}")

if __name__ == '__main__':
    export_transcript()
