<?php

/*
 * Copyright (C) 2015-2017 Franco Fichtner <franco@opnsense.org>
 * Copyright (C) 2009 Erik Kristensen <erik@erikkristensen.com>
 * Copyright (C) 2004-2010 Scott Ullrich <sullrich@gmail.com>
 * Copyright (C) 2003-2004 Manuel Kasper <mk@neon1.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* XXX make this a getter function for traceability */
$g = json_decode(file_get_contents('/usr/local/opnsense/firmware-product'), true);

openlog($g['product_id'], LOG_ODELAY, LOG_USER);
register_shutdown_function('closelog');

require_once("xmlparse.inc");
require_once("crypt.inc");
require_once("notices.inc");
require_once("legacy_bindings.inc");
require_once("certs.inc");

/*
 * Hook up the plugin system which consists of several low-profile
 * functions that can be called from within our backend code when
 * they exist.
 */
require_once('plugins.inc');

/* make a global alias table (for faster lookups) */
function alias_make_table($config)
{
    global $aliastable;

    $aliastable = array();

    if (isset($config['aliases']['alias'])) {
        foreach ($config['aliases']['alias'] as $alias) {
            if ($alias['name']) {
                $aliastable[$alias['name']] = isset($alias['address']) ? $alias['address'] : null;
            }
        }
    }
}

/**
 * parse config into array and return
 */
function load_config_from_file($filename)
{
    return OPNsense\Core\Config::getInstance()->toArrayFromFile($filename, listtags());
}

/****f* config/parse_config
 * NAME
 *   parse_config - Read in config.xml if needed and return $config array
 * RESULT
 *   $config      - array containing all configuration variables
 ******/
function parse_config()
{
    $cnf = OPNsense\Core\Config::getInstance();

    // return config data as array, use old "listags" construction to mark certain elements as array (even if they're not recurring)
    $config=$cnf->toArray(listtags());

    /* make alias table (for faster lookups) */
    alias_make_table($config);

    return $config;
}

/****f* config/convert_config
 * NAME
 *   convert_config - Attempt to update config.xml.
 * DESCRIPTION
 *   convert_config() reads the current global configuration
 *   and attempts to convert it to conform to the latest
 *   config.xml version. This allows major formatting changes
 *   to be made with a minimum of breakage.
 * RESULT
 *   null
 ******/
/* convert configuration, if necessary */
function convert_config($verbose = false)
{
    global $config;

    if (!isset($config['revision'])) {
        /* force a revision tag for proper handling in config history */
        write_config('Factory configuration', false);
    }

    /* chain the new migration into this function call */
    $mvc_migration = '/usr/local/opnsense/mvc/script/run_migrations.php';
    if ($verbose) {
        passthru($mvc_migration);
    } else {
        mwexecf($mvc_migration);
    }

    /* reload the config as it was rewritten and saved in the script context */
    OPNsense\Core\Config::getInstance()->forceReload();
    $config = parse_config();
}

/****f* config/write_config
 * NAME
 *   write_config - Backup and write the firewall configuration.
 * DESCRIPTION
 *   write_config() handles backing up the current configuration,
 *   applying changes, and regenerating the configuration cache.
 * INPUTS
 *   $desc  - string containing the a description of configuration changes
 *   $backup  - boolean: do not back up current configuration if false.
 * RESULT
 *   null
 ******/
/* save the system configuration */
function write_config($desc = '', $backup = true)
{
    global $config;

    if (!empty($_SERVER['REMOTE_ADDR'])) {
        if (!empty($_SESSION['Username']) && ($_SESSION['Username'] != 'root')) {
            $user = getUserEntry($_SESSION['Username']);
            if (is_array($user) && userHasPrivilege($user, "user-config-readonly")) {
                // okay, it's not very nice to check permissions here, but let's make it explicit while we do...
                log_error("WARNING: User {$_SESSION['Username']} may not write config (user-config-readonly set)");
                return false;
            }
        }
    }

    if (!isset($config['version'])) {
        // Examine config.xml, if for some strange reason the content is unexpected : exit directly.
        log_error('WARNING: Corrupt config!');
        return -1;
    }

    plugins_interfaces();

    $cnf = OPNsense\Core\Config::getInstance();
    $cnf->fromArray($config);
    $revision_info = make_config_revision_entry($desc);

    try {
        $cnf->save($revision_info, $backup);
    } catch (OPNsense\Core\ConfigException $e) {
        // write failure
        syslog(LOG_ERR, 'WARNING: Config contents could not be saved. Could not open file!');
        return -1;
    }

    /* sync carp entries to other firewalls */
    if ( isset($config['hasync']['synchronizetoip']) && trim($config['hasync']['synchronizetoip']) != "") {
        configd_run('filter sync load');
    }

    /* cleanup backups */
    cleanup_backups();

    // on succesfull save, serialize config back to global.
    $config = $cnf->toArray(listtags());
    return $config;
}

function config_restore($conffile)
{
    global $config;

    if (!file_exists($conffile))
      return 1;

    $cnf = OPNsense\Core\Config::getInstance();
    $cnf->backup();
    $cnf->restoreBackup($conffile);

    disable_security_checks();

    $config = parse_config();

    write_config(sprintf('Reverted to %s', array_pop(explode('/', $conffile))), false);

    return 0;
}

/*
 * Disable security checks for DNS rebind and HTTP referrer until next time
 * they pass (or reboot), to aid in preventing accidental lockout when
 * restoring settings like hostname, domain, IP addresses, and settings
 * related to the DNS rebind and HTTP referrer checks.
 * Intended for use when restoring a configuration or directly
 * modifying config.xml without an unconditional reboot.
 */
function disable_security_checks()
{
    touch('/tmp/disable_security_checks');
}

/* Restores security checks.  Should be called after all succeed. */
function restore_security_checks()
{
    @unlink('/tmp/disable_security_checks');
}

/* Returns status of security check temporary disable. */
function security_checks_disabled()
{
    return file_exists('/tmp/disable_security_checks');
}

/**
 * remove old backups
 */
function cleanup_backups()
{
    global $config;

    if (isset($config['system']['backupcount']) && is_numeric($config['system']['backupcount']) && ($config['system']['backupcount'] >= 0)) {
        $revisions = intval($config['system']['backupcount']);
    } else {
        /* XXX this value used to be left out of the config */
        $revisions = 60;
    }

    $cnf = OPNsense\Core\Config::getInstance();

    $cnt=1;
    foreach ($cnf->getBackups() as $filename) {
        if ($cnt > $revisions ) {
            @unlink($filename);
        }
        ++$cnt ;
    }
}

function &config_read_array()
{
    global $config;

    $current = &$config;

    foreach (func_get_args() as $key) {
        if (!isset($current[$key]) || !is_array($current[$key])) {
            $current[$key] = array();
        }
        $current = &$current[$key];
    }

    return $current;
}

function make_config_revision_entry($desc = '')
{
    if (!empty($_SESSION['Username'])) {
        $username = $_SESSION['Username'];
    } else {
        $username = '(' . trim(shell_exec('/usr/bin/whoami')) . ')';
    }

    if (!empty($_SERVER['REMOTE_ADDR'])) {
        $username .= '@' . $_SERVER['REMOTE_ADDR'];
    }

    if (empty($desc)) {
        $desc = sprintf('%s made changes', $_SERVER['SCRIPT_NAME']);
    }

    $revision = array();
    $revision['username'] = $username;
    $revision['time'] = microtime(true);
    $revision['description'] = $desc;

    return $revision;
}

/**
 * backup config to google drive and return current file list (/ info)
 *
 */
function backup_to_google_drive()
{
    $cnf = OPNsense\Core\Config::getInstance();
    if ($cnf->isValid()) {
        $config = $cnf->object();
        if (isset($config->system->remotebackup) && isset($config->system->remotebackup->GDriveEnabled) && $config->system->remotebackup->GDriveEnabled == "on") {
            try {
                $client = new Google\API\Drive();
                $client->login($config->system->remotebackup->GDriveEmail->__toString(),
                $config->system->remotebackup->GDriveP12key->__toString());
            } catch (Exception $e) {
                log_error("error connecting to Google Drive");
                return array();
            }

            // backup source data to local strings (plain/encrypted)
            $confdata = file_get_contents('/conf/config.xml');
            $confdata_enc = encrypt_data($confdata, $config->system->remotebackup->GDrivePassword->__toString());
            tagfile_reformat($confdata_enc, $confdata_enc, "config.xml");


            // read filelist (config-*.xml)
            try {
                $files = $client->listFiles($config->system->remotebackup->GDriveFolderID->__toString());
            } catch (Exception $e) {
                log_error("error while fetching filelist from Google Drive");
                return array();
            }

            $configfiles = array();
            foreach ($files as $file) {
                if (fnmatch("config-*.xml", $file['title'])) {
                    $configfiles[$file['title']] = $file;
                }
            }
            krsort($configfiles);


            // backup new file if changed (or if first in backup)
            $target_filename = "config-" . time() . ".xml";
            if (count($configfiles) > 1) {
                // compare last backup with current, only save new
                try {
                    $bck_data_enc_in = $client->download($configfiles[array_keys($configfiles)[0]]);
                    $bck_data_enc = "";
                    tagfile_deformat($bck_data_enc_in, $bck_data_enc, "config.xml");
                    $bck_data = decrypt_data($bck_data_enc, $config->system->remotebackup->GDrivePassword->__toString());
                    if ($bck_data == $confdata) {
                        $target_filename = null;
                    }
                } catch (Exception $e) {
                    log_error("unable to download " . $configfiles[array_keys($configfiles)[0]]->description . " from Google Drive (" . $e . ")");
                }
            }
            if (!is_null($target_filename)) {
                log_error("backup configuration as " . $target_filename);
                try {
                    $configfiles[$target_filename] = $client->upload($config->system->remotebackup->GDriveFolderID->__toString(), $target_filename, $confdata_enc);
                } catch (Exception $e) {
                    log_error("unable to upload " . $target_filename . " to Google Drive (" . $e . ")");
                    return array();
                }

                krsort($configfiles);
            }

            // cleanup old files
            if (isset($config->system->remotebackup->GDriveBackupCount) && is_numeric($config->system->remotebackup->GDriveBackupCount->__toString())) {
                $fcount = 0;
                foreach ($configfiles as $filename => $file) {
                    if ($fcount >= $config->system->remotebackup->GDriveBackupCount->__toString()) {
                        log_error("remove " . $filename . " from Google Drive");
                        try {
                            $client->delete($file);
                        } catch (Google_Service_Exception $e) {
                            log_error("unable to remove " . $filename . " from Google Drive");
                        }
                    }
                    $fcount++;
                }
            }

            // return filelist
            return $configfiles;
        }
    }

    // not configured / issue, return empty list
    return array();
}

/**
 * find list of registered interfaces
 * @param array $filters list of filters to apply
 * @return array interfaces
 */
function legacy_config_get_interfaces($filters = array())
{
    global $config;
    $interfaces = array();
    if (isset($config['interfaces'])) {
        foreach ($config['interfaces'] as $ifname => $iface) {
            // undo stupid listags() turning our item into a new array, preventing certain names to be used as interface.
            // see src/etc/inc/xmlparse.inc
            if (isset($iface[0])) {
                $iface = $iface[0];
            }
            // apply filters
            $iface_match = true;
            foreach ($filters as $filter_key => $filter_value) {
                if ($filter_key == 'enable' && isset($iface[$filter_key])) {
                    $field_value = true;
                } else {
                    $field_value = isset($iface[$filter_key]) ? $iface[$filter_key] : false;
                }
                if ($field_value != $filter_value) {
                    $iface_match = false;
                    break;
                }
            }
            if ($iface_match) {
                $iface['descr'] = !empty($iface['descr']) ? $iface['descr'] : strtoupper($ifname);
                $interfaces[$ifname] = $iface;
            }
        }
    }
    uasort($interfaces, function($a, $b) {
        return strnatcmp($a['descr'], $b['descr']);
    });
    return $interfaces;
}

$config = parse_config();

/* set timezone */
$timezone = $config['system']['timezone'];
if (!$timezone) {
    $timezone = 'Etc/UTC';
}

date_default_timezone_set($timezone);
