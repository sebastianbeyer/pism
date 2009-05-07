#!/usr/bin/env python
import re
from os import popen, system

input = "ice_bib"
output = "doxybib.txt"

# dummy LaTeX document that \cites everything and uses a special BibTeX style file:
latexdummy = """\\documentclass{article}
\\begin{document}
\\cite{*}\\bibliography{%s}\\bibliographystyle{doxybib}
\\end{document}
""" % input

bbl = "texput.bbl"

# Remove an old .bbl so that LaTeX does not choke on it:
system("rm -f %s" % bbl)

# Run LaTeX:
f= popen("latex", 'w')
f.write(latexdummy)
f.close()

# Run BibTeX:
system("bibtex texput")

# Read all the lines from a .bbl generated by BibTeX
f = open(bbl)
lines = f.readlines()
f.close()
lines = "".join(lines)

# NB! The order of substitutions is important.
subs = [(r"%\n",                      r""), # lines wrapped by BibTeX
        (r"\\href{([^}]*)}{([^}]*)}", r'<a href="\1">\2</a>'), # hyperref href command
        (r"\\url{([^}]*)}",           r'<a href="\1">\1</a>'), # hyperref url command
        (r"\\\w*{([^}]*)}",           r" \1 "),                # ignore other LaTeX commands
        (r"[}{]",                     r""),                    # curly braces
        (r"\$\\sim\$",                r"~"),                   # LaTeX \sim used to represent ~
        (r"---",                      r"&mdash;"),             # em-dash
        (r"--",                       r"&ndash;"),             # en-dash
        (r"([^/])~",                  r"\1&nbsp;"),            # tildes that are not in URLs
        (r'\\"([a-zA-Z])',            r"&\1uml;"),             # umlaut
        (r"\\'([a-zA-Z])",            r"&\1grave;"),           # grave
        (r'\\`([a-zA-Z])',            r"&\1acute;"),           # acute
        (r'\\^([a-zA-Z])',            r"&\1circ;"),            # circumflex
        ]

for (regex, substitution) in subs:
    r = re.compile(regex)
    lines = r.sub(substitution, lines)

f = open(output, 'w')
f.write(lines)
f.close()
