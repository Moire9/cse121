import flask

app = flask.Flask("weather_server")

@app.route("/", methods = ["POST"])
def post_recv():
	datas = flask.request.get_data(as_text=True).strip().split("\n")
	print(f"Onboard temperature: {datas[0]}°C\n{datas[1]}")
	return ""

@app.route("/location", methods = ["GET"])
def location():
	print("We are in Santa+Cruz")
	return "Santa+Cruz"

if __name__ == "__main__":
	app.run(host = "0.0.0.0", port = 1234)