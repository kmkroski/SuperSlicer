import os
from shutil import rmtree
from urllib.request import Request, urlopen, urlretrieve
import json
try:
	import requests
except ImportError:
	print("you need to do 'python -m pip install requests'");
	exit(0);
import zipfile
import io
import time
from datetime import date
import tarfile
import subprocess

repo = "supermerill/SuperSlicer"
program_name = "SuperSlicer"
path_7zip = r"C:\Program Files\7-Zip\7z.exe"
github_auth_token = "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"

def get_version():
	settings_stream = open("./version.inc", mode="r", encoding="utf-8");
	lines = settings_stream.read().splitlines();
	for line in lines:
		if("SLIC3R_VERSION_FULL" in line):
			elems = line.split("\"");
			return elems[1];
	return "";
	
	
date_str = date.today().strftime('%y%m%d');
version = get_version();
print("create release for: " + str(version));
release_path = "./build/release_"+str(version);
if(os.path.isdir(release_path)):
	rmtree(release_path);
	print("deleting old directory");
os.mkdir(release_path);
#urllib.urlretrieve ("https://api.github.com/repos/"+repo+"/actions/artifacts", release_path+"artifacts.json");
with urlopen("https://api.github.com/repos/"+repo+"/actions/artifacts") as f:
	artifacts = json.loads(f.read().decode('utf-8'));
	found_win = False; 
	found_linux = False; 
	found_linux_appimage = False; 
	found_macos = False; 
	print("there is "+ str(artifacts["total_count"])+ " artifacts in the repo");
	for entry in artifacts["artifacts"]:
		if entry["name"] == "rc_win64" and not found_win:
			found_win = True;
			print("ask for: "+entry["archive_download_url"]);
			resp = requests.get(entry["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("win: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content))
			base_name = release_path+"/"+program_name+"_"+version+"_win64_"+date_str;
			z.extractall(base_name);
			try:
				ret = subprocess.check_output([path_7zip, "a", "-tzip", base_name+".zip", base_name]);
			except:
				print("Failed to zip the win directory, do it yourself");
		if entry["name"] == "rc_macos.dmg" and not found_macos:
			found_macos = True;
			print("ask for: "+entry["archive_download_url"]);
			resp = requests.get(entry["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("macos: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content));
			z.extractall(release_path);
			os.rename(release_path+"/"+program_name+".dmg", release_path+"/"+program_name+"_"+version+"_macos_"+date_str+".dmg");
		if entry["name"] == "rc-"+program_name+"-AppImage.tar" and not found_linux_appimage:
			found_linux_appimage = True;
			print("ask for: "+entry["archive_download_url"]);
			resp = requests.get(entry["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("appimage: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content));
			z.extractall(release_path);
			my_tar = tarfile.open(release_path+"/"+program_name+"_ubu64.AppImage.tar");
			my_tar.extractall(release_path);
			my_tar.close();
			os.remove(release_path+"/"+program_name+"_ubu64.AppImage.tar");
			os.rename(release_path+"/"+program_name+"_ubu64.AppImage", release_path+"/"+program_name+"-ubuntu_18.04-" + version + ".AppImage");
		if entry["name"] == "rc_linux.tar" and not found_linux:
			found_linux = True;
			print("ask for: "+entry["archive_download_url"]);
			resp = requests.get(entry["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("appimage: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content));
			z.extractall(release_path);
			base_path = release_path+"/"+program_name+"_" + version + "_linux64_" + date_str;
			os.rename(release_path+"/"+program_name+".tar", base_path+".tar");
			try:
				subprocess.check_output([path_7zip, "a", "-tzip", base_path+".tar.zip", base_path+".tar"]);
				os.remove(base_path+".tar");
			except:
				with zipfile.ZipFile(base_path+"_bof.tar.zip", 'w') as myzip:
					myzip.write(base_path+".tar");
