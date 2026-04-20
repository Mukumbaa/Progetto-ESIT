import json
import boto3
import time
from boto3.dynamodb.conditions import Key

# Inizializzo il client AWS
dynamodb = boto3.resource('dynamodb')
iot_client = boto3.client('iot-data')

def lambda_handler(event, context):
    device_id = event['deviceId']
    timestamp = int(event['timestamp'])
    
    # Recupera l'area del sensore dal registro
    registry_table = dynamodb.Table('SensorsRegistry')
    sensor_info = registry_table.get_item(Key={'deviceId': device_id})
    if 'Item' not in sensor_info:
        print(f"Errore: {device_id} non registrato!")
        return
    area_id = sensor_info['Item']['areaId']

    # Salva il tilt corrente
    tilt_table = dynamodb.Table('TiltEvents')
    tilt_table.put_item(Item={
        'deviceId': device_id,
        'timestamp': timestamp,
        'areaId': area_id
    })

    # Leggo configurazioni da SystemSettings (Soglie e Intervalli Display)
    settings_table = dynamodb.Table('SystemSettings')
    # Cerco la configurazione 'GLOBAL'
    conf_res = settings_table.get_item(Key={'areaId': 'GLOBAL'})
    
    # Valori di default se la tabella è vuota
    settings = conf_res.get('Item', {
        'tiltThreshold': 3, 
        'timeWindow': 10,
        'int1': 1,   # Ore intervallo 1
        'int2': 12,  # Ore intervallo 2
        'int3': 24   # Ore intervallo 3
    })
    
    threshold = int(settings['tiltThreshold'])
    window = int(settings['timeWindow'])

    # calcolo frequenza
    start_time = timestamp - window
    response = tilt_table.scan(
        FilterExpression=Key('areaId').eq(area_id) & Key('timestamp').gte(start_time)
    )
    current_tilts = len(response['Items'])

# se soglia superata -> controlla se è un nuovo allarme o una replica
    if current_tilts >= threshold:
        # Controllo l'ultimo allarme salvato per questa area
        alarms_table = dynamodb.Table('AlarmsHistory')
        
        # Cerco l'ultimo allarme (ScanIndexForward=False prende il più recente)
        last_alarm_res = alarms_table.query(
            KeyConditionExpression=Key('areaId').eq(area_id),
            ScanIndexForward=False, 
            Limit=1
        )
        
        should_notify = True
        if last_alarm_res['Items']:
            last_timestamp = int(last_alarm_res['Items'][0]['timestamp'])
            # Se l'ultimo allarme è avvenuto meno di 'window' secondi fa, non notificare di nuovo
            if (timestamp - last_timestamp) < window:
                should_notify = False
                print("Allarme già inviato recentemente, salto la notifica.")

        if should_notify:
            print("!!! NUOVO ALLARME SISMA RILEVATO !!!")
            alarms_table.put_item(Item={
                'areaId': area_id,
                'timestamp': timestamp
            })
            
            # Invia notifica MQTT (Telegram la riceve da Node-RED)
            iot_client.publish(
                topic='alarms/notifications',
                payload=json.dumps({"msg": f"SISMA RILEVATO nell'area {area_id}!", "area": area_id})
            )
    # aggiorno i display di tutti i dispositivi
    update_system_shadows(settings)

    return {"status": "ok"}

def update_system_shadows(settings):
    alarms_table = dynamodb.Table('AlarmsHistory')
    registry_table = dynamodb.Table('SensorsRegistry')
    now = int(time.time())
    
    # Recupero le ore configurate dall'utente su Node-RED
    h1, h2, h3 = int(settings['int1']), int(settings['int2']), int(settings['int3'])
    
    # Funzione per contare allarmi totali del sistema nelle ultime X ore
    def count_total_alarms(hours):
        since_timestamp = now - (hours * 3600)
        # Scan senza filtri per areaId = conteggio di sistema
        res = alarms_table.scan(
            FilterExpression=Key('timestamp').gte(since_timestamp)
        )
        return len(res['Items'])

    # Calcoliamo i 3 valori dinamici
    c1 = count_total_alarms(h1)
    c2 = count_total_alarms(h2)
    c3 = count_total_alarms(h3)

    # Prendo tutti i dispositivi nel sistema
    all_devices = registry_table.scan()['Items']

    # payload Shadow
    # Includo anche i valori delle ore (h1, h2, h3) così il display può essere dinamico
    shadow_payload = {
        "state": {
            "desired": {
                "alarmCounts": {
                    "lastH1": {"label": h1, "count": c1},
                    "lastH2": {"label": h2, "count": c2},
                    "lastH3": {"label": h3, "count": c3}
                }
            }
        }
    }

    # Aggiorno ogni Thing registrato
    for dev in all_devices:
        try:
            iot_client.update_thing_shadow(
                thingName=dev['deviceId'],
                payload=json.dumps(shadow_payload)
            )
        except Exception as e:
            print(f"Errore aggiornamento shadow {dev['deviceId']}: {e}")
