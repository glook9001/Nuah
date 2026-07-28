#!/usr/bin/python3
'''
AOSPs framework-res contains a lot of drawables which are only for the systemUI. Only resources reachable
from public.xml are part of the Android API and should be kept. This script removes all other resources.
'''

import glob
import re
import os

# find all xml files and parse them for resource references
xml_files = []
resources = []
xml_files_new = glob.glob("res/values/public*.xml")
xml_files_new += glob.glob("res/values*/themes*.xml")
xml_files_new += glob.glob("res/values*/styles*.xml")
xml_files_new += glob.glob("res/values*/colors*.xml")
xml_files_new += glob.glob("res/xml*/*.xml")
xml_files_new += glob.glob("AndroidManifest.xml")
while len(xml_files_new) > 0:
	xml_files += xml_files_new
	xml_files_old = xml_files_new
	xml_files_new = []
	for f in xml_files_old:
		with open(f, "r") as x:
			str = x.read()
			resources_new = []
			for pattern in ['name="(.*?)"', '"@drawable/(.*?)"', '>@drawable/(.*?)<', '@drawable/(.*?)\n', '"@layout/(.*?)"', '>@layout/(.*?)<', '>@android:layout/(.*?)<', '"@color/(.*?)"', '"@anim/(.*?)"', '"@android:drawable/(.*?)"', '"@\\*android:drawable/(.*?)"']:
				for m in re.finditer(pattern, str):
					resources_new.append(m.group(1))
			resources += resources_new
			for name in resources_new:
				matches = glob.glob("res/**/" + name + ".xml")
				for m in matches:
					if not m in xml_files:
						xml_files_new.append(m)

# remove unused layouts
for f in glob.glob("res/layout*/*"):
	if not os.path.basename(f).split(".")[0] in resources:
		os.remove(f)

# remove unused drawables
for f in glob.glob("res/drawable*/*"):
	if not os.path.basename(f).split(".")[0] in resources:
		os.remove(f)
